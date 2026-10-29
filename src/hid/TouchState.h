#pragma once

#include <array>
#include <functional>
#include <optional>

#include "hid/Hid.h"

namespace xdb::hid {

// A successful release forgets only its contact. Failed sends retain enough
// state for cleanup, including a down report that may have reached the device.
class TouchState {
public:
    using Send = std::function<bool(std::span<const TouchContact>, std::string &)>;

    bool active() const {
        for (const auto &contact : contacts_) if (contact) return true;
        return false;
    }

    bool update(int identity, double x, double y, bool down, const Send &send, std::string &error) {
        if (identity < 0 || identity >= int(kMaxTouchContacts)) {
            error = "Touch contact must be an integer in [0,4]";
            return false;
        }
        // The original single-contact report used HID identity 2. Keep logical
        // contact 0 byte-compatible while assigning distinct IDs to the others.
        const auto wire_id = uint8_t(identity == 0 ? 2 : identity <= 2 ? identity - 1 : identity);
        const TouchContact contact{wire_id, normalize(x), normalize(y), down};
        auto pending = contacts_;
        pending[identity] = contact;
        if (down) contacts_[identity] = contact;
        std::vector<TouchContact> frame;
        for (const auto &entry : pending) if (entry) frame.push_back(*entry);
        if (!send(frame, error)) return false;
        if (!down) contacts_[identity].reset();
        return true;
    }

    bool release(const Send &send, std::string &error) {
        std::vector<TouchContact> frame;
        for (const auto &entry : contacts_) {
            if (!entry) continue;
            auto contact = *entry;
            contact.down = false;
            frame.push_back(contact);
        }
        if (frame.empty()) return true;
        if (!send(frame, error)) return false;
        contacts_ = {};
        return true;
    }

private:
    std::array<std::optional<TouchContact>, kMaxTouchContacts> contacts_{};
};

} // namespace xdb::hid
