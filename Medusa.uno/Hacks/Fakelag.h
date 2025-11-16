#pragma once

namespace Fakelag
{
    void run(bool& sendPacket) noexcept;
    inline int latest_choked_packets{};
}