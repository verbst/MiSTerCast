#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Estimates receiver consumption from the display's field counter.
class ReceiverAudioClock
{
public:
    struct Counters { uint64_t staleReports = 0, reanchors = 0; } counters;

    void reset(unsigned rate, unsigned bufferMs, double fieldMs, uint32_t frame, int64_t now)
    {
        m_rate = rate;
        m_lead = int64_t(rate) * (std::min)(bufferMs / 2, 40u) / 1000;
        m_samplesPerField = double(rate) * fieldMs / 1000.0;
        m_field100ns = fieldMs * 10000.0;
        m_baseFrame = m_lastFrame = frame;
        m_lastCall = m_lastReport = now;
        m_played = m_offset = 0;
        m_due = m_lead;
        m_stale = false;
        counters = {};
    }

    int64_t due(uint32_t frame, int64_t now)
    {
        if (!m_rate)
            return 0;
        if (frame > m_lastFrame)
        {
            double observed = double(frame - m_baseFrame) * m_samplesPerField + m_offset;
            if (std::abs(observed - m_played) > double(m_rate) / 10.0)
            {
                m_offset += m_played - observed;
                observed = m_played;
                ++counters.reanchors;
            }
            m_played = observed;
            m_lastFrame = frame;
            m_lastReport = now;
            m_stale = false;
        }
        else
        {
            m_played += double((std::max)(int64_t(0), now - m_lastCall)) * m_rate / 10000000.0;
            if (!m_stale && double(now - m_lastReport) > 2.0 * m_field100ns)
            {
                m_stale = true;
                ++counters.staleReports;
            }
        }
        m_lastCall = now;
        m_due = (std::max)(m_due, int64_t(std::llround(m_played)) + m_lead);
        return m_due;
    }

    int64_t played() const { return int64_t(std::llround(m_played)); }
    int64_t lead() const { return m_lead; }
    bool stale() const { return m_stale; }

    void reanchor(uint32_t frame, int64_t now, int64_t outputFrames)
    {
        m_baseFrame = m_lastFrame = frame;
        m_lastCall = m_lastReport = now;
        m_played = double(outputFrames);
        m_offset = 0;
        m_due = outputFrames + m_lead;
        m_stale = false;
        ++counters.reanchors;
    }

private:
    unsigned m_rate = 0;
    uint32_t m_baseFrame = 0, m_lastFrame = 0;
    int64_t m_lastCall = 0, m_lastReport = 0, m_lead = 0, m_due = 0;
    double m_samplesPerField = 0, m_field100ns = 0, m_played = 0, m_offset = 0;
    bool m_stale = false;
};

// Packet timestamps place PCM; the receiver clock determines output quantity.
class AudioTimeline
{
public:
    struct Counters
    {
        uint64_t insertedSilence = 0, late = 0, rejected = 0, skipped = 0, rebased = 0;
        uint64_t maxSendGap100ns = 0;
        double rateCorrectionFrames = 0;
    } counters;

    void reset(unsigned rate, unsigned delayMs, int64_t now)
    {
        m_rate = rate;
        m_epoch = now;
        m_delay = int64_t(rate) * delayMs / 1000;
        m_lead = int64_t(rate) * (std::min)(delayMs / 2, 40u) / 1000;
        m_sourcePhase = 0;
        m_output = 0;
        m_pending = 0;
        m_positionValid = false;
        m_lastRender = now;
        m_lastPacketEnd = 0;
        m_ratePpm = 0;
        counters = {};
        m_slots.assign((std::max)(size_t(1), size_t(rate) * (delayMs + 250) / 1000), Slot{});
    }

    void push(const float* pcm, unsigned frames, unsigned channels, bool silent,
        uint64_t position, int64_t timestamp, bool valid, bool discontinuity)
    {
        if (!m_rate || !valid || (!silent && !pcm) || !channels)
        {
            counters.rejected += frames;
            m_positionValid = false;
            return;
        }
        int64_t start = sampleAt(timestamp) + m_delay;
        // Preserve contiguous samples despite timestamp rounding and small device jitter.
        if (m_positionValid && !discontinuity && position == m_expectedPosition &&
            std::abs(start - m_packetEnd) <= int64_t(m_rate) / 500)
            start = m_packetEnd;
        else if (m_positionValid)
            ++counters.rebased;
        m_packetEnd = start + frames;
        m_expectedPosition = position + frames;
        m_positionValid = true;
        m_lastPacketEnd = timestamp + int64_t(frames) * 10000000 / m_rate;

        for (unsigned i = 0; i < frames; ++i)
        {
            const int64_t index = start + i;
            if (index < int64_t(m_sourcePhase))
            {
                ++counters.late;
                continue;
            }
            if (index - int64_t(m_sourcePhase) >= int64_t(m_slots.size()))
            {
                ++counters.rejected;
                continue;
            }
            Slot& slot = m_slots[size_t(index) % m_slots.size()];
            if (slot.index != index)
                ++m_pending;
            slot.index = index;
            slot.left = silent ? 0 : convert(pcm[size_t(i) * channels]);
            slot.right = silent ? 0 : convert(pcm[size_t(i) * channels + (channels > 1 ? 1 : 0)]);
        }
    }

    unsigned render(int64_t now, int64_t due, int16_t* output, unsigned capacityFrames)
    {
        if (!m_rate || !output || !capacityFrames || due <= m_output)
            return 0;
        const bool fresh = m_lastPacketEnd && now - m_lastPacketEnd < 1000000;
        const double error = double(sampleAt(now) + m_lead) -
            (m_sourcePhase + double(due - m_output));
        const double correction = fresh ? (std::max)(-0.001, (std::min)(0.001, error / (m_rate * 10.0))) : 0.0;
        const double step = 1.0 + correction;
        m_ratePpm = correction * 1000000.0;

        if (due - m_output > capacityFrames)
        {
            const int64_t skipped = due - m_output - capacityFrames;
            advance(double(skipped) * step);
            counters.skipped += skipped;
            m_output += skipped;
        }

        counters.maxSendGap100ns = (std::max)(counters.maxSendGap100ns,
            uint64_t((std::max)(int64_t(0), now - m_lastRender)));
        m_lastRender = now;
        unsigned count = 0;
        while (m_output < due)
        {
            const int64_t index = int64_t(m_sourcePhase);
            const double fraction = m_sourcePhase - double(index);
            const Slot& first = m_slots[size_t(index) % m_slots.size()];
            const Slot& second = m_slots[size_t(index + 1) % m_slots.size()];
            if (first.index == index)
            {
                const bool nextAvailable = second.index == index + 1;
                output[count * 2] = nextAvailable ? interpolate(first.left, second.left, fraction) : first.left;
                output[count * 2 + 1] = nextAvailable ? interpolate(first.right, second.right, fraction) : first.right;
            }
            else
            {
                output[count * 2] = output[count * 2 + 1] = 0;
                ++counters.insertedSilence;
            }
            advance(step);
            counters.rateCorrectionFrames += correction;
            ++count;
            ++m_output;
        }
        return count;
    }

    unsigned pending() const { return m_pending; }
    int64_t outputFrames() const { return m_output; }
    double ratePpm() const { return m_ratePpm; }
    double sourceErrorFrames(int64_t now) const { return double(sampleAt(now) + m_lead) - m_sourcePhase; }

    void rebaseToNow(int64_t now)
    {
        const double target = double(sampleAt(now));
        if (target > m_sourcePhase)
            advance(target - m_sourcePhase);
    }

private:
    struct Slot { int64_t index = -1; int16_t left = 0, right = 0; };
    std::vector<Slot> m_slots;
    unsigned m_rate = 0, m_pending = 0;
    int64_t m_epoch = 0, m_delay = 0, m_lead = 0, m_packetEnd = 0, m_lastRender = 0, m_lastPacketEnd = 0;
    int64_t m_output = 0;
    double m_sourcePhase = 0, m_ratePpm = 0;
    uint64_t m_expectedPosition = 0;
    bool m_positionValid = false;

    void advance(double frames)
    {
        const int64_t oldIndex = int64_t(m_sourcePhase);
        m_sourcePhase += frames;
        const int64_t newIndex = int64_t(m_sourcePhase);
        if (newIndex - oldIndex >= int64_t(m_slots.size()))
        {
            for (Slot& slot : m_slots)
                slot.index = -1;
            m_pending = 0;
        }
        else
        {
            for (int64_t index = oldIndex; index < newIndex; ++index)
            {
                Slot& slot = m_slots[size_t(index) % m_slots.size()];
                if (slot.index == index)
                {
                    slot.index = -1;
                    --m_pending;
                }
            }
        }
    }

    int64_t sampleAt(int64_t timestamp) const
    {
        const int64_t elapsed = timestamp - m_epoch;
        return (elapsed / 10000000) * m_rate + (elapsed % 10000000) * m_rate / 10000000;
    }

    static int16_t interpolate(int16_t first, int16_t second, double fraction)
    {
        return static_cast<int16_t>(std::lround(first + (second - first) * fraction));
    }

    static int16_t convert(float sample)
    {
        if (!std::isfinite(sample)) return 0;
        return static_cast<int16_t>((std::max)(-1.0f, (std::min)(1.0f, sample)) * 32767.0f);
    }
};
