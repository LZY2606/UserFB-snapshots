#include "send_scheduler.hh"

#include <chrono>
#include <thread>

static int64_t steady_now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void steady_wait_until_ns(int64_t deadline_ns)
{
    int64_t remaining = deadline_ns - steady_now_ns();

    if (remaining > 0) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(remaining));
    }
}

uvgrtp::send_scheduler::send_scheduler():
    fps_valid_(false),
    frame_interval_ns_(0),
    pace_numerator_(8),
    pace_denominator_(10),
    sync_valid_(false),
    sync_point_ns_(0),
    slots_since_sync_(0),
    now_hook_(steady_now_ns),
    wait_hook_(steady_wait_until_ns)
{}

void uvgrtp::send_scheduler::set_fps(ssize_t numerator, ssize_t denominator)
{
    fps_valid_ = numerator > 0 && denominator > 0;

    if (fps_valid_) {
        frame_interval_ns_ = (int64_t)(uint64_t)(1.0 / double(numerator / denominator) * 1000 * 1000 * 1000);
    }

    /* the next access unit establishes a new synchronization point */
    sync_valid_ = false;
    slots_since_sync_ = 0;
}

void uvgrtp::send_scheduler::set_pace(ssize_t numerator, ssize_t denominator)
{
    if (numerator > 0 && denominator > 0) {
        pace_numerator_ = numerator;
        pace_denominator_ = denominator;
    }
    else {
        pace_numerator_ = 8;
        pace_denominator_ = 10;
    }
}

void uvgrtp::send_scheduler::set_timing_hooks(std::function<int64_t()> now_ns,
    std::function<void(int64_t)> wait_until_ns)
{
    if (now_ns && wait_until_ns) {
        now_hook_ = std::move(now_ns);
        wait_hook_ = std::move(wait_until_ns);
    }
    else {
        /* restore the default clock and waiting mechanism */
        now_hook_ = steady_now_ns;
        wait_hook_ = steady_wait_until_ns;
    }
}

bool uvgrtp::send_scheduler::fps_valid() const
{
    return fps_valid_;
}

int64_t uvgrtp::send_scheduler::begin_access_unit(bool use_frame_rate)
{
    int64_t now = now_ns();

    if (!use_frame_rate) {
        /* pacing-only access units start at the calling moment */
        return now;
    }

    int64_t slot = sync_point_ns_ + (int64_t)(slots_since_sync_ * (uint64_t)frame_interval_ns_);

    if (!sync_valid_ || now > slot) {
        /* first access unit or we are late: rebuild the schedule from this
         * moment, this resynchronization is done only once for this call */
        sync_point_ns_ = now;
        slots_since_sync_ = 0;
        sync_valid_ = true;
        slot = now;
    }

    wait_until(slot);
    ++slots_since_sync_; // this access unit occupies one frame slot

    return slot;
}

int64_t uvgrtp::send_scheduler::packet_deadline(int64_t au_start_ns, uint64_t index, uint64_t packet_count) const
{
    if (packet_count == 0) {
        return au_start_ns;
    }

    uint64_t numerator = (uint64_t)index * (uint64_t)frame_interval_ns_ * (uint64_t)pace_numerator_;
    uint64_t denominator = (uint64_t)packet_count * (uint64_t)pace_denominator_;

    return au_start_ns + (int64_t)(numerator / denominator);
}

void uvgrtp::send_scheduler::wait_until(int64_t deadline_ns) const
{
    wait_hook_(deadline_ns);
}

int64_t uvgrtp::send_scheduler::now_ns() const
{
    return now_hook_();
}
