/**
 * @file src/raw_hid_tablet.h
 * @brief Session-scoped raw HID tablet redirection.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "thread_safe.h"

namespace raw_hid {
  using feedback_queue_t = safe::mail_raw_t::queue_t<std::vector<std::uint8_t>>;

  /**
   * @brief Check whether this process can create Linux UHID devices.
   *
   * @return True when raw tablet redirection can be offered to a client.
   */
  bool available();

  /**
   * @brief Input-core state of one mirrored tablet input node.
   */
  struct input_node_state_t {
    std::vector<std::uint16_t> held_keys;  ///< Key and button codes the input core reports as down.
    std::optional<std::int32_t> pressure;  ///< ABS_PRESSURE value when the node reports pressure.
    std::optional<std::int32_t> mt_current_slot;  ///< Current ABS_MT_SLOT when the node is multitouch.
    std::vector<std::int32_t> mt_tracking_ids;  ///< ABS_MT_TRACKING_ID of every slot on a multitouch node.
  };

  /**
   * @brief One event injected into a mirrored tablet input node.
   */
  struct release_event_t {
    std::uint16_t type;  ///< Linux input event type.
    std::uint16_t code;  ///< Linux input event code.
    std::int32_t value;  ///< Event value.

    bool operator==(const release_event_t &) const = default;
  };

  /**
   * @brief Plan the events that end pen, button, key and touch contact on one node.
   *
   * Tool proximity keys are left alone: hid-wacom tracks tool proximity itself,
   * and clearing it behind the driver would drop the next proximity-in. Every
   * released control is sent again with the first real report after resume.
   *
   * @param state Current state of one mirrored input node.
   * @return Events ending with SYN_REPORT, or none when nothing is held.
   */
  std::vector<release_event_t> plan_contact_release(const input_node_state_t &state);

  /**
   * @brief Owns virtual HID interfaces created for one streaming session.
   */
  class tablet_t {
  public:
    /**
     * @brief Create an empty tablet redirector.
     *
     * @param feedback_queue Queue carrying host control requests to the client.
     */
    explicit tablet_t(feedback_queue_t feedback_queue);

    /**
     * @brief Destroy all virtual interfaces owned by this session.
     */
    ~tablet_t();

    tablet_t(const tablet_t &) = delete;
    tablet_t &operator=(const tablet_t &) = delete;

    /**
     * @brief Process one complete PLANK raw HID wire frame.
     *
     * @param frame Header and payload received from the authenticated client.
     * @return True when the frame was valid and accepted.
     */
    bool handle(const std::vector<std::uint8_t> &frame);

    /**
     * @brief Bind outbound control messages to a resumed session mailbox.
     *
     * @param feedback_queue Queue carrying host control requests to the client.
     */
    void rebind(feedback_queue_t feedback_queue);

    /**
     * @brief Suspend transport delivery while retaining stable UHID endpoints.
     *
     * Held pen, button, key and touch contact on the retained endpoints is
     * released so a suspend mid-stroke cannot leave a stuck contact. A resumed
     * client must present the same device identity and report descriptors
     * before input delivery is enabled again.
     */
    void suspend();

    /**
     * @brief Destroy every virtual interface and discard pending state.
     */
    void reset();

    /**
     * @brief Return whether exact raw-HID endpoints currently exist.
     *
     * Suspended endpoints remain present so their XInput identities survive a
     * resumable focus or transport transition.
     *
     * @return True when one or more UHID endpoints are retained.
     */
    bool has_endpoints();

#ifdef SUNSHINE_TESTS
    /**
     * @brief Return the active generation for lifecycle regression tests.
     */
    std::uint16_t active_generation();

    /**
     * @brief Return the endpoint creation epoch for reconnect regression tests.
     */
    std::uint64_t endpoint_epoch();
#endif

  private:
    class impl_t;
    std::unique_ptr<impl_t> impl_;  ///< Platform implementation and session state.
  };
}  // namespace raw_hid
