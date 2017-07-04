/*
 * Copyright © 2015 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Kevin DuBois <kevin.dubois@canonical.com>
 */

#include "mir/test/doubles/mock_event_sink.h"
#include "mir/test/fake_shared.h"
#include "mir/test/doubles/stub_buffer_allocator.h"
#include "src/server/compositor/multi_monitor_arbiter.h"
#include "src/server/compositor/schedule.h"
#include "src/server/compositor/temporary_buffers.h"

#include <gtest/gtest.h>
using namespace testing;
namespace mt = mir::test;
namespace mtd = mir::test::doubles;
namespace mc = mir::compositor;
namespace mg = mir::graphics;
namespace mf = mir::frontend;

namespace
{
struct FixedSchedule : mc::Schedule
{
    void schedule(std::shared_ptr<mg::Buffer> const&) override
    {
        throw std::runtime_error("this stub doesnt support this");
    }
    std::future<void> schedule_nonblocking(
        std::shared_ptr<mg::Buffer> const&) override
    {
        throw std::runtime_error("this stub doesnt support this");
        return {};
    }
    unsigned int num_scheduled() override
    {
        return sched.size() - current;
    }
    std::shared_ptr<mg::Buffer> next_buffer() override
    {
        if (sched.empty() || current == sched.size())
            throw std::runtime_error("no buffer scheduled");
        auto buf = sched.front();
        sched.erase(sched.begin());
        return buf;
    }
    void set_schedule(std::vector<std::shared_ptr<mg::Buffer>> s)
    {
        current = 0;
        sched = s;
    }
private:
    unsigned int current{0};
    std::vector<std::shared_ptr<mg::Buffer>> sched;
};

struct MultiMonitorArbiter : Test
{
    MultiMonitorArbiter()
    {
        for(auto i = 0u; i < num_buffers; i++)
            buffers.emplace_back(std::make_shared<mtd::StubBuffer>());
    }
    unsigned int const num_buffers{6u};
    std::vector<std::shared_ptr<mg::Buffer>> buffers;
    FixedSchedule schedule;
    mc::MultiMonitorArbiter arbiter{mt::fake_shared(schedule)};
};

MATCHER_P(IsSameBufferAs, buffer, "")
{
    return buffer->id() == arg->id();
}

std::shared_ptr<mg::Buffer> wrap_with_destruction_notifier(
    std::shared_ptr<mg::Buffer> const& buffer,
    std::shared_ptr<bool> const& destroyed)
{
    class DestructionNotifyingBuffer : public mc::TemporaryBuffer
    {
    public:
        DestructionNotifyingBuffer(
            std::shared_ptr<mg::Buffer> const& buffer,
            std::shared_ptr<bool> const& destroyed)
            : TemporaryBuffer(buffer),
              destroyed{destroyed}
        {
        }

        ~DestructionNotifyingBuffer()
        {
            *destroyed = true;
        }
    private:
        std::shared_ptr<bool> const destroyed;
    };

    return std::make_shared<DestructionNotifyingBuffer>(buffer, destroyed);
}
}

TEST_F(MultiMonitorArbiter, compositor_access_before_any_submission_throws)
{
    //nothing owned
    EXPECT_THROW({
        arbiter.compositor_acquire(this);
    }, std::logic_error);

    schedule.set_schedule({buffers[0]});

    //something scheduled, should be ok
    arbiter.compositor_acquire(this);
}

TEST_F(MultiMonitorArbiter, compositor_access)
{
    schedule.set_schedule({buffers[0]});
    auto cbuffer = arbiter.compositor_acquire(this);
    EXPECT_THAT(cbuffer, IsSameBufferAs(buffers[0]));
}

TEST_F(MultiMonitorArbiter, compositor_release_sends_buffer_back)
{
    auto buffer_released = std::make_shared<bool>(false);
    schedule.set_schedule({ wrap_with_destruction_notifier(buffers[0], buffer_released) });

    auto cbuffer = arbiter.compositor_acquire(this);
    schedule.set_schedule({buffers[1]});
    arbiter.compositor_release(cbuffer);
    cbuffer.reset();
    // We need to acquire a new buffer - the current one is on-screen, so can't be sent back.
    arbiter.compositor_acquire(this);
    EXPECT_TRUE(*buffer_released);
}

TEST_F(MultiMonitorArbiter, compositor_can_acquire_different_buffers)
{
    schedule.set_schedule({buffers[0]});
    auto cbuffer1 = arbiter.compositor_acquire(this);
    schedule.set_schedule({buffers[1]});
    auto cbuffer2 = arbiter.compositor_acquire(this);
    EXPECT_THAT(cbuffer1, Not(IsSameBufferAs(cbuffer2)));
}

TEST_F(MultiMonitorArbiter, compositor_buffer_syncs_to_fastest_compositor)
{
    int comp_id1{0};
    int comp_id2{0};

    schedule.set_schedule({buffers[0]});
    auto cbuffer1 = arbiter.compositor_acquire(&comp_id1); 
    auto cbuffer2 = arbiter.compositor_acquire(&comp_id2);

    schedule.set_schedule({buffers[1]});
    auto cbuffer3 = arbiter.compositor_acquire(&comp_id1);

    schedule.set_schedule({buffers[0]});
    auto cbuffer4 = arbiter.compositor_acquire(&comp_id1); 
    auto cbuffer5 = arbiter.compositor_acquire(&comp_id2);

    schedule.set_schedule({buffers[1]});
    auto cbuffer6 = arbiter.compositor_acquire(&comp_id2);
    auto cbuffer7 = arbiter.compositor_acquire(&comp_id2);

    EXPECT_THAT(cbuffer1, IsSameBufferAs(buffers[0]));
    EXPECT_THAT(cbuffer2, IsSameBufferAs(buffers[0]));
    EXPECT_THAT(cbuffer3, IsSameBufferAs(buffers[1]));
    EXPECT_THAT(cbuffer4, IsSameBufferAs(buffers[0]));
    EXPECT_THAT(cbuffer5, IsSameBufferAs(buffers[0]));
    EXPECT_THAT(cbuffer6, IsSameBufferAs(buffers[1]));
    EXPECT_THAT(cbuffer7, IsSameBufferAs(buffers[1]));
}

TEST_F(MultiMonitorArbiter, compositor_consumes_all_buffers_when_operating_as_a_composited_scene_would)
{
    schedule.set_schedule({buffers[0],buffers[1],buffers[2],buffers[3],buffers[4]});

    auto cbuffer1 = arbiter.compositor_acquire(this);
    arbiter.compositor_release(cbuffer1);
    auto cbuffer2 = arbiter.compositor_acquire(this);
    arbiter.compositor_release(cbuffer2);
    auto cbuffer3 = arbiter.compositor_acquire(this);
    arbiter.compositor_release(cbuffer3);
    auto cbuffer4 = arbiter.compositor_acquire(this);
    arbiter.compositor_release(cbuffer4);
    auto cbuffer5 = arbiter.compositor_acquire(this);
    arbiter.compositor_release(cbuffer5);

    EXPECT_THAT(cbuffer1, IsSameBufferAs(buffers[0]));
    EXPECT_THAT(cbuffer2, IsSameBufferAs(buffers[1]));
    EXPECT_THAT(cbuffer3, IsSameBufferAs(buffers[2]));
    EXPECT_THAT(cbuffer4, IsSameBufferAs(buffers[3]));
    EXPECT_THAT(cbuffer5, IsSameBufferAs(buffers[4]));
}

TEST_F(MultiMonitorArbiter, compositor_consumes_all_buffers_when_operating_as_a_bypassed_buffer_would)
{
    schedule.set_schedule({buffers[0],buffers[1],buffers[2],buffers[3],buffers[4]});

    auto cbuffer1 = arbiter.compositor_acquire(this);
    auto cbuffer2 = arbiter.compositor_acquire(this);
    arbiter.compositor_release(cbuffer1);
    auto cbuffer3 = arbiter.compositor_acquire(this);
    arbiter.compositor_release(cbuffer2);
    auto cbuffer4 = arbiter.compositor_acquire(this);
    arbiter.compositor_release(cbuffer3);
    auto cbuffer5 = arbiter.compositor_acquire(this);
    arbiter.compositor_release(cbuffer4);
    arbiter.compositor_release(cbuffer5);

    EXPECT_THAT(cbuffer1, IsSameBufferAs(buffers[0]));
    EXPECT_THAT(cbuffer2, IsSameBufferAs(buffers[1]));
    EXPECT_THAT(cbuffer3, IsSameBufferAs(buffers[2]));
    EXPECT_THAT(cbuffer4, IsSameBufferAs(buffers[3]));
    EXPECT_THAT(cbuffer5, IsSameBufferAs(buffers[4]));
}

TEST_F(MultiMonitorArbiter, multimonitor_compositor_buffer_syncs_to_fastest_with_more_queueing)
{
    int comp_id1{0};
    int comp_id2{0};

    schedule.set_schedule({buffers[0],buffers[1],buffers[2],buffers[3],buffers[4]});

    auto cbuffer1 = arbiter.compositor_acquire(&comp_id1); //buffer[0]
    auto cbuffer2 = arbiter.compositor_acquire(&comp_id2); //buffer[0]

    auto cbuffer3 = arbiter.compositor_acquire(&comp_id1); //buffer[1]

    auto cbuffer4 = arbiter.compositor_acquire(&comp_id1); //buffer[2]
    auto cbuffer5 = arbiter.compositor_acquire(&comp_id2); //buffer[2]

    auto cbuffer6 = arbiter.compositor_acquire(&comp_id2); //buffer[3]

    auto cbuffer7 = arbiter.compositor_acquire(&comp_id2); //buffer[4]
    auto cbuffer8 = arbiter.compositor_acquire(&comp_id1); //buffer[4]

    EXPECT_THAT(cbuffer1, IsSameBufferAs(buffers[0]));
    EXPECT_THAT(cbuffer2, IsSameBufferAs(buffers[0]));

    EXPECT_THAT(cbuffer3, IsSameBufferAs(buffers[1]));

    EXPECT_THAT(cbuffer4, IsSameBufferAs(buffers[2]));
    EXPECT_THAT(cbuffer5, IsSameBufferAs(buffers[2]));

    EXPECT_THAT(cbuffer6, IsSameBufferAs(buffers[3]));

    EXPECT_THAT(cbuffer7, IsSameBufferAs(buffers[4]));
    EXPECT_THAT(cbuffer8, IsSameBufferAs(buffers[4]));
}

TEST_F(MultiMonitorArbiter, can_set_a_new_schedule)
{
    FixedSchedule another_schedule;
    schedule.set_schedule({buffers[3],buffers[4]});
    another_schedule.set_schedule({buffers[0],buffers[1]});

    auto cbuffer1 = arbiter.compositor_acquire(this);
    arbiter.set_schedule(mt::fake_shared(another_schedule));
    auto cbuffer2 = arbiter.compositor_acquire(this);

    EXPECT_THAT(cbuffer1, IsSameBufferAs(buffers[3]));
    EXPECT_THAT(cbuffer2, IsSameBufferAs(buffers[0]));
}

TEST_F(MultiMonitorArbiter, basic_snapshot_equals_compositor_buffer)
{
    schedule.set_schedule({buffers[3],buffers[4]});

    auto cbuffer1 = arbiter.compositor_acquire(this);
    auto sbuffer1 = arbiter.snapshot_acquire();
    EXPECT_THAT(cbuffer1, IsSameBufferAs(sbuffer1));
}

TEST_F(MultiMonitorArbiter, basic_snapshot_equals_latest_compositor_buffer)
{
    schedule.set_schedule({buffers[3],buffers[4]});
    int that = 4;

    auto cbuffer1 = arbiter.compositor_acquire(this);
    auto cbuffer2 = arbiter.compositor_acquire(&that);
    auto sbuffer1 = arbiter.snapshot_acquire();
    arbiter.snapshot_release(sbuffer1);
    arbiter.compositor_release(cbuffer2);
    cbuffer2 = arbiter.compositor_acquire(&that);

    auto sbuffer2 = arbiter.snapshot_acquire();
    EXPECT_THAT(cbuffer1, IsSameBufferAs(sbuffer1));
    EXPECT_THAT(cbuffer2, IsSameBufferAs(sbuffer2));
}

TEST_F(MultiMonitorArbiter, snapshot_cycling_doesnt_advance_buffer_for_compositors)
{
    schedule.set_schedule({buffers[3],buffers[4]});
    auto that = 4;
    auto a_few_times = 5u;
    auto cbuffer1 = arbiter.compositor_acquire(this);
    std::vector<std::shared_ptr<mg::Buffer>> snapshot_buffers(a_few_times);
    for(auto i = 0u; i < a_few_times; i++)
    {
        auto b = arbiter.snapshot_acquire();
        arbiter.snapshot_release(b);
        snapshot_buffers[i] = b;
    }
    auto cbuffer2 = arbiter.compositor_acquire(&that);

    EXPECT_THAT(cbuffer1, IsSameBufferAs(cbuffer2));
    EXPECT_THAT(snapshot_buffers, Each(IsSameBufferAs(cbuffer1)));
}

TEST_F(MultiMonitorArbiter, no_buffers_available_throws_on_snapshot)
{
    schedule.set_schedule({});
    EXPECT_THROW({
        arbiter.snapshot_acquire();
    }, std::logic_error);
}

TEST_F(MultiMonitorArbiter, snapshotting_will_release_buffer_if_it_was_the_last_owner)
{
    auto buffer_released = std::make_shared<bool>(false);
    schedule.set_schedule(
        {
            wrap_with_destruction_notifier(buffers[3], buffer_released),
            buffers[4]
        });
    auto cbuffer1 = arbiter.compositor_acquire(this);
    auto sbuffer1 = arbiter.snapshot_acquire();
    arbiter.compositor_release(cbuffer1);
    cbuffer1.reset();

    // Acquire a new buffer so first one is no longer onscreen.
    arbiter.compositor_acquire(this);

    EXPECT_FALSE(*buffer_released);
    arbiter.snapshot_release(sbuffer1);
    sbuffer1.reset();
    EXPECT_TRUE(*buffer_released);
}

TEST_F(MultiMonitorArbiter, compositor_can_acquire_a_few_times_and_only_sends_on_the_last_release)
{
    int comp_id1{0};
    int comp_id2{0};

    auto buffer_released = std::make_shared<bool>(false);
    schedule.set_schedule(
        {
            wrap_with_destruction_notifier(buffers[0], buffer_released),
            buffers[1]
        });
    auto cbuffer1 = arbiter.compositor_acquire(&comp_id1);
    auto cbuffer2 = arbiter.compositor_acquire(&comp_id2);
    EXPECT_THAT(cbuffer1, IsSameBufferAs(cbuffer2));

    auto cbuffer3 = arbiter.compositor_acquire(&comp_id1);
    arbiter.compositor_release(cbuffer2);
    EXPECT_FALSE(*buffer_released);
    arbiter.compositor_release(cbuffer1);
    cbuffer1.reset();
    cbuffer2.reset();
    EXPECT_TRUE(*buffer_released);
}

TEST_F(MultiMonitorArbiter, advance_on_fastest_has_same_buffer)
{
    int comp_id1{0};
    int comp_id2{0};
    schedule.set_schedule({buffers[0],buffers[1]});

    auto cbuffer1 = arbiter.compositor_acquire(&comp_id1); //buffer[0]
    arbiter.compositor_release(cbuffer1);
    auto cbuffer2 = arbiter.compositor_acquire(&comp_id2); //buffer[0]
    arbiter.compositor_release(cbuffer2);

    auto cbuffer3 = arbiter.compositor_acquire(&comp_id1); //buffer[1]
 
    EXPECT_THAT(cbuffer1, IsSameBufferAs(cbuffer2));
    EXPECT_THAT(cbuffer1, IsSameBufferAs(buffers[0]));
    EXPECT_THAT(cbuffer3, IsSameBufferAs(buffers[1]));
}

TEST_F(MultiMonitorArbiter, buffers_are_sent_back)
{
    std::array<std::shared_ptr<bool>, 3> buffer_released = {
        {
            std::make_shared<bool>(false),
            std::make_shared<bool>(false),
            std::make_shared<bool>(false)
        }};
    int comp_id1{0};
    int comp_id2{0};

    schedule.set_schedule(
        {
            wrap_with_destruction_notifier(buffers[0], buffer_released[0]),
            wrap_with_destruction_notifier(buffers[1], buffer_released[1]),
            wrap_with_destruction_notifier(buffers[2], buffer_released[2]),
            buffers[3]
        });

    auto b1 = arbiter.compositor_acquire(&comp_id1);
    arbiter.compositor_release(b1);
    auto b2 = arbiter.compositor_acquire(&comp_id1);
    arbiter.compositor_release(b2);
    auto b3 = arbiter.compositor_acquire(&comp_id1);
    auto b5 = arbiter.compositor_acquire(&comp_id2);
    arbiter.compositor_release(b3);
    auto b4 = arbiter.compositor_acquire(&comp_id1);
    arbiter.compositor_release(b5);
    arbiter.compositor_release(b4);
    auto b6 = arbiter.compositor_acquire(&comp_id1);
    arbiter.compositor_release(b6);

    b1.reset();
    b2.reset();
    b3.reset();
    b4.reset();
    b5.reset();
    b6.reset();

    EXPECT_THAT(buffer_released, Each(Pointee(true)));
}

TEST_F(MultiMonitorArbiter, can_check_if_buffers_are_ready)
{
    int comp_id1{0};
    int comp_id2{0};
    schedule.set_schedule({buffers[3]});

    EXPECT_TRUE(arbiter.buffer_ready_for(&comp_id1));
    EXPECT_TRUE(arbiter.buffer_ready_for(&comp_id2));

    auto b1 = arbiter.compositor_acquire(&comp_id1);
    EXPECT_FALSE(arbiter.buffer_ready_for(&comp_id1));
    EXPECT_TRUE(arbiter.buffer_ready_for(&comp_id2));
    arbiter.compositor_release(b1);

    auto b2 = arbiter.compositor_acquire(&comp_id2);
    EXPECT_FALSE(arbiter.buffer_ready_for(&comp_id1));
    EXPECT_FALSE(arbiter.buffer_ready_for(&comp_id2));
    arbiter.compositor_release(b2);
} 

TEST_F(MultiMonitorArbiter, other_compositor_ready_status_advances_with_fastest_compositor)
{
    int comp_id1{0};
    int comp_id2{0};
    schedule.set_schedule({buffers[0], buffers[1], buffers[2]});

    EXPECT_TRUE(arbiter.buffer_ready_for(&comp_id1));
    EXPECT_TRUE(arbiter.buffer_ready_for(&comp_id2));

    auto b1 = arbiter.compositor_acquire(&comp_id1);
    arbiter.compositor_release(b1);
    EXPECT_TRUE(arbiter.buffer_ready_for(&comp_id1));
    EXPECT_TRUE(arbiter.buffer_ready_for(&comp_id2));

    b1 = arbiter.compositor_acquire(&comp_id1);
    arbiter.compositor_release(b1);
    EXPECT_TRUE(arbiter.buffer_ready_for(&comp_id1));
    EXPECT_TRUE(arbiter.buffer_ready_for(&comp_id2));

    b1 = arbiter.compositor_acquire(&comp_id1);
    arbiter.compositor_release(b1);
    EXPECT_FALSE(arbiter.buffer_ready_for(&comp_id1));
    EXPECT_TRUE(arbiter.buffer_ready_for(&comp_id2));

    b1 = arbiter.compositor_acquire(&comp_id2);
    arbiter.compositor_release(b1);
    EXPECT_FALSE(arbiter.buffer_ready_for(&comp_id1));
    EXPECT_FALSE(arbiter.buffer_ready_for(&comp_id2));
}

TEST_F(MultiMonitorArbiter, will_release_buffer_in_nbuffers_2_overlay_scenario)
{
    int comp_id1{0};
    auto buffer_released = std::make_shared<bool>(false);
    auto notifying_buffer = wrap_with_destruction_notifier(buffers[0], buffer_released);
    schedule.set_schedule(
        {
            notifying_buffer,
            buffers[1],
            notifying_buffer,
            buffers[1]
        });
    notifying_buffer.reset();

    auto b1 = arbiter.compositor_acquire(&comp_id1);
    auto b2 = arbiter.compositor_acquire(&comp_id1);
    EXPECT_THAT(b1, IsSameBufferAs(buffers[0]));
    EXPECT_THAT(b2, IsSameBufferAs(buffers[1]));
    arbiter.compositor_release(b1);
    arbiter.compositor_release(b2);
    b1.reset();
    b2.reset();
    arbiter.compositor_acquire(&comp_id1);

    EXPECT_TRUE(buffer_released);
}

TEST_F(MultiMonitorArbiter, will_release_buffer_in_nbuffers_2_starvation_scenario)
{
    int comp_id1{0};
    int comp_id2{0};
    schedule.set_schedule({buffers[0], buffers[1], buffers[0], buffers[1]});

    auto b1 = arbiter.compositor_acquire(&comp_id1);
    auto b2 = arbiter.compositor_acquire(&comp_id1);
    arbiter.compositor_release(b1);

    auto b3 = arbiter.compositor_acquire(&comp_id2);
    auto b4 = arbiter.compositor_acquire(&comp_id2);
    arbiter.compositor_release(b3);

    arbiter.compositor_release(b2);
    arbiter.compositor_release(b4);

    EXPECT_THAT(b1, IsSameBufferAs(buffers[0]));
    EXPECT_THAT(b2, IsSameBufferAs(buffers[1]));
    EXPECT_THAT(b3, IsSameBufferAs(buffers[1]));
    EXPECT_THAT(b4, IsSameBufferAs(buffers[0]));

} 

TEST_F(MultiMonitorArbiter, will_ensure_smooth_monitor_production)
{
    int comp_id1{0};
    int comp_id2{0};

    schedule.set_schedule({
        buffers[0], buffers[1], buffers[2],
        buffers[0], buffers[1], buffers[2],
        buffers[0], buffers[1], buffers[2]});

    auto b1 = arbiter.compositor_acquire(&comp_id1);
    auto b2 = arbiter.compositor_acquire(&comp_id2);
    arbiter.compositor_release(b1); //send nothing

    auto b3 = arbiter.compositor_acquire(&comp_id1);
    arbiter.compositor_release(b3); //send nothing

    auto b4 = arbiter.compositor_acquire(&comp_id2);
    arbiter.compositor_release(b2); //send 0

    auto b5 = arbiter.compositor_acquire(&comp_id1);
    arbiter.compositor_release(b5); //send nothing

    EXPECT_THAT(b1, IsSameBufferAs(buffers[0]));
    EXPECT_THAT(b2, IsSameBufferAs(buffers[0]));
    EXPECT_THAT(b3, IsSameBufferAs(buffers[1]));
    EXPECT_THAT(b4, IsSameBufferAs(buffers[1]));
    EXPECT_THAT(b5, IsSameBufferAs(buffers[2]));
}

TEST_F(MultiMonitorArbiter, can_advance_buffer_manually)
{
    int comp_id1{0};
    int comp_id2{0};
    schedule.set_schedule({buffers[0], buffers[1], buffers[2]});

    arbiter.advance_schedule();
    arbiter.advance_schedule();

    auto b1 = arbiter.compositor_acquire(&comp_id1);
    auto b2 = arbiter.compositor_acquire(&comp_id2);
    EXPECT_THAT(b1, IsSameBufferAs(buffers[1]));
    EXPECT_THAT(b2, IsSameBufferAs(buffers[1]));

    auto b3 = arbiter.compositor_acquire(&comp_id1);
    EXPECT_THAT(b3, IsSameBufferAs(buffers[2]));
}

TEST_F(MultiMonitorArbiter, checks_if_buffer_is_valid_after_clean_onscreen_buffer)
{
    int comp_id1{0};

    schedule.set_schedule({buffers[0], buffers[1], buffers[2], buffers[3]});

    arbiter.advance_schedule();
    arbiter.advance_schedule();
    arbiter.advance_schedule();
    arbiter.advance_schedule();

    auto b1 = arbiter.compositor_acquire(&comp_id1);

    EXPECT_THAT(b1->id(), Eq(buffers[3]->id()));
    EXPECT_THAT(b1->size(), Eq(buffers[3]->size()));
}

TEST_F(MultiMonitorArbiter, releases_buffer_on_destruction)
{
    auto buffer_released = std::make_shared<bool>(false);
    schedule.set_schedule({wrap_with_destruction_notifier(buffers[0], buffer_released)});

    {
        mc::MultiMonitorArbiter arbiter{mt::fake_shared(schedule)};
        arbiter.advance_schedule();
    }
    EXPECT_TRUE(*buffer_released);
}
