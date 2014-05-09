/*
 * Copyright © 2012-2014 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Alan Griffiths <alan@octopull.co.uk>
 */

#include "session_mediator.h"
#include "client_buffer_tracker.h"

#include "mir/frontend/session_mediator_report.h"
#include "mir/frontend/shell.h"
#include "mir/frontend/session.h"
#include "mir/frontend/surface.h"
#include "mir/scene/surface_creation_parameters.h"
#include "mir/frontend/display_changer.h"
#include "resource_cache.h"
#include "mir_toolkit/common.h"
#include "mir/graphics/buffer_id.h"
#include "mir/graphics/buffer.h"
#include "mir/compositor/buffer_stream.h"
#include "mir/geometry/dimensions.h"
#include "mir/graphics/platform.h"
#include "mir/frontend/display_changer.h"
#include "mir/graphics/display_configuration.h"
#include "mir/graphics/pixel_format_utils.h"
#include "mir/graphics/platform_ipc_package.h"
#include "mir/graphics/drm_authenticator.h"
#include "mir/frontend/client_constants.h"
#include "mir/frontend/event_sink.h"
#include "mir/frontend/screencast.h"
#include "mir/frontend/trust_session.h"
#include "mir/scene/trust_session_creation_parameters.h"

#include "mir/geometry/rectangles.h"
#include "client_buffer_tracker.h"
#include "protobuf_buffer_packer.h"

#include <boost/exception/get_error_info.hpp>
#include <boost/exception/errinfo_errno.hpp>
#include <boost/throw_exception.hpp>

#include <mutex>
#include <functional>

namespace ms = mir::scene;
namespace mf = mir::frontend;
namespace mfd=mir::frontend::detail;
namespace mg = mir::graphics;
namespace geom = mir::geometry;

mf::SessionMediator::SessionMediator(
    pid_t client_pid,
    std::shared_ptr<frontend::Shell> const& shell,
    std::shared_ptr<graphics::Platform> const & graphics_platform,
    std::shared_ptr<mf::DisplayChanger> const& display_changer,
    std::vector<MirPixelFormat> const& surface_pixel_formats,
    std::shared_ptr<SessionMediatorReport> const& report,
    std::shared_ptr<EventSink> const& sender,
    std::shared_ptr<ResourceCache> const& resource_cache,
    std::shared_ptr<Screencast> const& screencast,
    ConnectionContext const& connection_context) :
    client_pid(client_pid),
    shell(shell),
    graphics_platform(graphics_platform),
    surface_pixel_formats(surface_pixel_formats),
    display_changer(display_changer),
    report(report),
    event_sink(sender),
    resource_cache(resource_cache),
    screencast(screencast),
    connection_context(connection_context)
{
}

mf::SessionMediator::~SessionMediator() noexcept
{
    if (auto session = weak_session.lock())
    {
        report->session_error(session->name(), __PRETTY_FUNCTION__, "connection dropped without disconnect");
        shell->close_session(session);
    }
}

void mf::SessionMediator::connect(
    ::google::protobuf::RpcController*,
    const ::mir::protobuf::ConnectParameters* request,
    ::mir::protobuf::Connection* response,
    ::google::protobuf::Closure* done)
{
    report->session_connect_called(request->application_name());

    auto const session = shell->open_session(client_pid, request->application_name(), event_sink);
    {
        std::lock_guard<std::mutex> lock(session_mutex);
        weak_session = session;
    }
    connection_context.handle_client_connect(session);

    auto ipc_package = graphics_platform->get_ipc_package();
    auto platform = response->mutable_platform();

    for (auto& data : ipc_package->ipc_data)
        platform->add_data(data);

    for (auto& ipc_fds : ipc_package->ipc_fds)
        platform->add_fd(ipc_fds);

    auto display_config = display_changer->active_configuration();
    auto protobuf_config = response->mutable_display_configuration();
    mfd::pack_protobuf_display_configuration(*protobuf_config, *display_config);

    for (auto pf : surface_pixel_formats)
        response->add_surface_pixel_format(static_cast<::google::protobuf::uint32>(pf));

    resource_cache->save_resource(response, ipc_package);

    done->Run();
}

void mf::SessionMediator::advance_buffer(
    SurfaceId surf_id,
    Surface& surface,
    std::function<void(graphics::Buffer*, bool)> complete)
{
    auto& tracker = client_buffer_tracker[surf_id];
    if (!tracker) tracker = std::make_shared<ClientBufferTracker>(client_buffer_cache_size);

    auto& client_buffer = client_buffer_resource[surf_id];
    surface.swap_buffers(client_buffer,
        [&tracker, &client_buffer, complete](mg::Buffer* new_buffer)
        {
            client_buffer = new_buffer;
            auto id = client_buffer->id();
            auto need_full_ipc = !tracker->client_has(id);
            tracker->add(id);

            complete(client_buffer, need_full_ipc);
        });
}


void mf::SessionMediator::create_surface(
    google::protobuf::RpcController* /*controller*/,
    const mir::protobuf::SurfaceParameters* request,
    mir::protobuf::Surface* response,
    google::protobuf::Closure* done)
{

    auto const lock = std::make_shared<std::unique_lock<std::mutex>>(session_mutex);

    auto const session = weak_session.lock();

    if (session.get() == nullptr)
        BOOST_THROW_EXCEPTION(std::logic_error("Invalid application session"));

    report->session_create_surface_called(session->name());

    auto const surf_id = session->create_surface(ms::SurfaceCreationParameters()
        .of_name(request->surface_name())
        .of_size(request->width(), request->height())
        .of_buffer_usage(static_cast<graphics::BufferUsage>(request->buffer_usage()))
        .of_pixel_format(static_cast<MirPixelFormat>(request->pixel_format()))
        .with_output_id(graphics::DisplayConfigurationOutputId(request->output_id())));

    auto surface = session->get_surface(surf_id);
    response->mutable_id()->set_value(surf_id.as_value());
    response->set_width(surface->size().width.as_uint32_t());
    response->set_height(surface->size().height.as_uint32_t());
    response->set_pixel_format((int)surface->pixel_format());
    response->set_buffer_usage(request->buffer_usage());

    if (surface->supports_input())
        response->add_fd(surface->client_input_fd());

    advance_buffer(surf_id, *surface,
        [lock, this, response, done, session](graphics::Buffer* client_buffer, bool need_full_ipc)
        {
            lock->unlock();

            auto buffer = response->mutable_buffer();
            pack_protobuf_buffer(*buffer, client_buffer, need_full_ipc);

            // TODO: NOTE: We use the ordering here to ensure the shell acts on the surface after the surface ID is sent over the wire.
            // This guarantees that notifications such as, gained focus, etc, can be correctly interpreted by the client.
            // To achieve this order we rely on done->Run() sending messages synchronously. As documented in mfd::SocketMessenger::send.
            // this will require additional synchronization if mfd::SocketMessenger::send changes.
            done->Run();
            shell->handle_surface_created(session);
        });
}

void mf::SessionMediator::next_buffer(
    ::google::protobuf::RpcController* /*controller*/,
    ::mir::protobuf::SurfaceId const* request,
    ::mir::protobuf::Buffer* response,
    ::google::protobuf::Closure* done)
{
    SurfaceId const surf_id{request->value()};

    auto const lock = std::make_shared<std::unique_lock<std::mutex>>(session_mutex);

    auto const session = weak_session.lock();

    if (session.get() == nullptr)
        BOOST_THROW_EXCEPTION(std::logic_error("Invalid application session"));

    report->session_next_buffer_called(session->name());

    auto surface = session->get_surface(surf_id);

    advance_buffer(surf_id, *surface,
        [lock, this, response, done, session](graphics::Buffer* client_buffer, bool need_full_ipc)
        {
            lock->unlock();

            pack_protobuf_buffer(*response, client_buffer, need_full_ipc);

            done->Run();
        });
}

void mf::SessionMediator::release_surface(
    google::protobuf::RpcController* /*controller*/,
    const mir::protobuf::SurfaceId* request,
    mir::protobuf::Void*,
    google::protobuf::Closure* done)
{
    {
        std::unique_lock<std::mutex> lock(session_mutex);

        auto session = weak_session.lock();

        if (session.get() == nullptr)
            BOOST_THROW_EXCEPTION(std::logic_error("Invalid application session"));

        report->session_release_surface_called(session->name());

        auto const id = SurfaceId(request->value());

        session->destroy_surface(id);
        client_buffer_tracker.erase(id);
    }

    // TODO: We rely on this sending responses synchronously.
    done->Run();
}

void mf::SessionMediator::disconnect(
    google::protobuf::RpcController* /*controller*/,
    const mir::protobuf::Void* /*request*/,
    mir::protobuf::Void* /*response*/,
    google::protobuf::Closure* done)
{
    {
        std::unique_lock<std::mutex> lock(session_mutex);

        auto session = weak_session.lock();

        if (session.get() == nullptr)
            BOOST_THROW_EXCEPTION(std::logic_error("Invalid application session"));

        report->session_disconnect_called(session->name());

        shell->close_session(session);
        weak_session.reset();
    }

    done->Run();
}

void mf::SessionMediator::configure_surface(
    google::protobuf::RpcController*, // controller,
    const mir::protobuf::SurfaceSetting* request,
    mir::protobuf::SurfaceSetting* response,
    google::protobuf::Closure* done)
{
    MirSurfaceAttrib attrib = static_cast<MirSurfaceAttrib>(request->attrib());

    // Required response fields:
    response->mutable_surfaceid()->CopyFrom(request->surfaceid());
    response->set_attrib(attrib);

    {
        std::unique_lock<std::mutex> lock(session_mutex);

        auto session = weak_session.lock();

        if (session.get() == nullptr)
            BOOST_THROW_EXCEPTION(std::logic_error("Invalid application session"));

        report->session_configure_surface_called(session->name());

        auto const id = frontend::SurfaceId(request->surfaceid().value());
        int value = request->ivalue();
        auto const surface = session->get_surface(id);
        int newvalue = surface->configure(attrib, value);

        response->set_ivalue(newvalue);
    }

    done->Run();
}

void mf::SessionMediator::configure_display(
    ::google::protobuf::RpcController*,
    const ::mir::protobuf::DisplayConfiguration* request,
    ::mir::protobuf::DisplayConfiguration* response,
    ::google::protobuf::Closure* done)
{
    {
        std::unique_lock<std::mutex> lock(session_mutex);
        auto session = weak_session.lock();

        if (session.get() == nullptr)
            BOOST_THROW_EXCEPTION(std::logic_error("Invalid application session"));

        report->session_configure_display_called(session->name());

        auto config = display_changer->active_configuration();

        config->for_each_output([&](mg::UserDisplayConfigurationOutput& dest){
            unsigned id = dest.id.as_value();
            int n = 0;
            for (; n < request->display_output_size(); ++n)
            {
                if (request->display_output(n).output_id() == id)
                    break;
            }
            if (n >= request->display_output_size())
                return;

            auto& src = request->display_output(n);
            dest.used = src.used();
            dest.top_left = geom::Point{src.position_x(),
                                        src.position_y()};
            dest.current_mode_index = src.current_mode();
            dest.current_format =
                static_cast<MirPixelFormat>(src.current_format());
            dest.power_mode = static_cast<MirPowerMode>(src.power_mode());
            dest.orientation = static_cast<MirOrientation>(src.orientation());
        });

        display_changer->configure(session, config);
        auto display_config = display_changer->active_configuration();
        mfd::pack_protobuf_display_configuration(*response, *display_config);
    }
    done->Run();
}

void mf::SessionMediator::create_screencast(
    google::protobuf::RpcController*,
    const mir::protobuf::ScreencastParameters* parameters,
    mir::protobuf::Screencast* protobuf_screencast,
    google::protobuf::Closure* done)
{
    static bool const need_full_ipc{true};

    geom::Rectangle const region{
        {parameters->region().left(), parameters->region().top()},
        {parameters->region().width(), parameters->region().height()}
    };
    geom::Size const size{parameters->width(), parameters->height()};
    MirPixelFormat const pixel_format = static_cast<MirPixelFormat>(parameters->pixel_format());

    auto screencast_session_id = screencast->create_session(region, size, pixel_format);
    auto buffer = screencast->capture(screencast_session_id);

    protobuf_screencast->mutable_screencast_id()->set_value(
        screencast_session_id.as_value());
    pack_protobuf_buffer(*protobuf_screencast->mutable_buffer(),
                         buffer.get(),
                         need_full_ipc);

    done->Run();
}

void mf::SessionMediator::release_screencast(
    google::protobuf::RpcController*,
    const mir::protobuf::ScreencastId* protobuf_screencast_id,
    mir::protobuf::Void*,
    google::protobuf::Closure* done)
{
    ScreencastSessionId const screencast_session_id{
        protobuf_screencast_id->value()};
    screencast->destroy_session(screencast_session_id);
    done->Run();
}

void mf::SessionMediator::screencast_buffer(
    google::protobuf::RpcController*,
    const mir::protobuf::ScreencastId* protobuf_screencast_id,
    mir::protobuf::Buffer* protobuf_buffer,
    google::protobuf::Closure* done)
{
    static bool const does_not_need_full_ipc{false};
    ScreencastSessionId const screencast_session_id{
        protobuf_screencast_id->value()};

    auto buffer = screencast->capture(screencast_session_id);

    pack_protobuf_buffer(*protobuf_buffer,
                         buffer.get(),
                         does_not_need_full_ipc);

    done->Run();
}

void mf::SessionMediator::new_fds_for_trusted_clients(
    ::google::protobuf::RpcController* ,
    ::mir::protobuf::SocketFDRequest const* parameters,
    ::mir::protobuf::SocketFD* response,
    ::google::protobuf::Closure* done)
{
    {
        std::unique_lock<std::mutex> lock(session_mutex);
        auto session = weak_session.lock();

        if (session.get() == nullptr)
            BOOST_THROW_EXCEPTION(std::logic_error("Invalid application session"));

        auto trust_session = weak_trust_session.lock();
        if (trust_session.get() == nullptr)
            BOOST_THROW_EXCEPTION(std::logic_error("Invalid trust session"));

        // TODO write a handler that connects the new session to our trust session
        auto const connect_handler = [this](std::shared_ptr<frontend::Session> const& session)
        {
            auto trust_session = weak_trust_session.lock();
            if (trust_session.get() == nullptr)
                BOOST_THROW_EXCEPTION(std::logic_error("Invalid trust session"));

            shell->add_trusted_session_for(trust_session, session->process_id());
        };

        auto const fds_requested = parameters->number();

        // < 1 is illogical, > 42 is unreasonable
        if (fds_requested < 1 || fds_requested > 42)
            BOOST_THROW_EXCEPTION(std::runtime_error("number of fds requested out of range"));

        for (auto i  = 0; i != fds_requested; ++i)
        {
            auto const fd = connection_context.fd_for_new_client(connect_handler);
            response->add_fd(fd);
        }
    }

    done->Run();
}

void mf::SessionMediator::drm_auth_magic(
    google::protobuf::RpcController* /*controller*/,
    const mir::protobuf::DRMMagic* request,
    mir::protobuf::DRMAuthMagicStatus* response,
    google::protobuf::Closure* done)
{
    {
        std::unique_lock<std::mutex> lock(session_mutex);
        auto session = weak_session.lock();

        if (session.get() == nullptr)
            BOOST_THROW_EXCEPTION(std::logic_error("Invalid application session"));

        report->session_drm_auth_magic_called(session->name());
    }

    auto const magic = static_cast<unsigned int>(request->magic());
    auto authenticator = std::dynamic_pointer_cast<mg::DRMAuthenticator>(graphics_platform);
    if (!authenticator)
        BOOST_THROW_EXCEPTION(std::logic_error("drm_auth_magic request not supported by the active platform"));

    try
    {
        authenticator->drm_auth_magic(magic);
        response->set_status_code(0);
    }
    catch (std::exception const& e)
    {
        auto errno_ptr = boost::get_error_info<boost::errinfo_errno>(e);

        if (errno_ptr != nullptr)
            response->set_status_code(*errno_ptr);
        else
            throw;
    }

    done->Run();
}

void mf::SessionMediator::start_trust_session(::google::protobuf::RpcController*,
    const ::mir::protobuf::TrustSessionParameters* request,
    ::mir::protobuf::TrustSession* response,
    ::google::protobuf::Closure* done)
{
    {
        std::unique_lock<std::mutex> lock(session_mutex);
        auto session = weak_session.lock();

        if (session.get() == nullptr)
            BOOST_THROW_EXCEPTION(std::logic_error("Invalid application session"));

        ms::TrustSessionCreationParameters parameters;
        parameters.set_base_process_id(request->base_trusted_session().pid());

        std::ostringstream stream;
        stream << "process id: " << parameters.base_process_id;
        report->session_start_trust_session_called(session->name(), stream.str());

        auto current_trust_session = weak_trust_session.lock();
        if (current_trust_session.get() != nullptr)
            BOOST_THROW_EXCEPTION(std::runtime_error("Cannot start another trust session"));

        auto trust_session = shell->start_trust_session_for(session, parameters);
        weak_trust_session = trust_session;

        if (trust_session)
        {
            response->set_state(trust_session->get_state());
        }
    }
    done->Run();
}

void mf::SessionMediator::add_trusted_session(::google::protobuf::RpcController*,
    const ::mir::protobuf::TrustedSession* request,
    ::mir::protobuf::TrustSessionAddResult* response,
    ::google::protobuf::Closure* done)
{
    {
        std::unique_lock<std::mutex> lock(session_mutex);
        auto session = weak_session.lock();

        if (session.get() == nullptr)
            BOOST_THROW_EXCEPTION(std::logic_error("Invalid application session"));

        auto trust_session = weak_trust_session.lock();

        if (trust_session.get() == nullptr)
            BOOST_THROW_EXCEPTION(std::logic_error("Invalid trust session"));

        std::ostringstream stream;
        stream << "process id: " << request->pid();
        report->session_add_trusted_session_called(session->name(), stream.str());

        response->set_result(shell->add_trusted_session_for(trust_session, request->pid()));
    }
    done->Run();
}

void mf::SessionMediator::stop_trust_session(::google::protobuf::RpcController*,
    const ::mir::protobuf::Void*,
    ::mir::protobuf::Void*,
    ::google::protobuf::Closure* done)
{
    {
        std::unique_lock<std::mutex> lock(session_mutex);
        auto session = weak_session.lock();

        if (session.get() == nullptr)
            BOOST_THROW_EXCEPTION(std::logic_error("Invalid application session"));

        auto trust_session = weak_trust_session.lock();
        weak_trust_session.reset();

        if (trust_session.get() == nullptr)
            BOOST_THROW_EXCEPTION(std::logic_error("Invalid trusted session"));

        report->session_stop_trust_session_called(session->name());

        shell->stop_trust_session(trust_session);
    }
    done->Run();
}

void mf::SessionMediator::pack_protobuf_buffer(
    protobuf::Buffer& protobuf_buffer,
    graphics::Buffer* graphics_buffer,
    bool need_full_ipc)
{
    protobuf_buffer.set_buffer_id(graphics_buffer->id().as_uint32_t());

    if (need_full_ipc)
    {
        mfd::ProtobufBufferPacker packer{&protobuf_buffer};
        graphics_platform->fill_ipc_package(&packer, graphics_buffer);
    }
}
