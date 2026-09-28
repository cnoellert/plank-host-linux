/**
 * @file src/platform/virtualhid_input.h
 * @brief Declarations for libvirtualhid-backed input helpers.
 */
#pragma once

// standard includes
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

// lib includes
#include <libvirtualhid/libvirtualhid.hpp>

// local includes
#include "src/platform/common.h"

namespace platf::virtualhid {

  /**
   * @brief Runtime and virtual devices owned by one platform input context.
   */
  struct input_context_t {
    /**
     * @brief Construct the libvirtualhid input context using the platform-default backend.
     */
    input_context_t();

    /**
     * @brief Construct the libvirtualhid input context using a selected backend.
     *
     * @param backend Backend used to create the libvirtualhid runtime.
     */
    explicit input_context_t(lvh::BackendKind backend);

    std::unique_ptr<lvh::Runtime> runtime;  ///< libvirtualhid runtime.
    std::unique_ptr<lvh::Keyboard> keyboard;  ///< Shared virtual keyboard.
    std::unique_ptr<lvh::Mouse> mouse;  ///< Shared virtual mouse.
  };

  /**
   * @brief Per-client virtual pen state.
   */
  struct client_context_t {
    /**
     * @brief Create per-client libvirtualhid devices.
     *
     * @param input Global input context.
     */
    explicit client_context_t(input_context_t &input);

    input_context_t *global = nullptr;  ///< Shared global input context.
    std::unique_ptr<lvh::PenTablet> pen;  ///< Per-client pen tablet.
    std::set<lvh::PenButton> pressed_pen_buttons;  ///< Active pen tablet buttons.
  };

  /**
   * @brief Enable or remove the normalized pen-tablet fallback.
   *
   * Exact raw-HID tablet forwarding must be the only tablet exposed while it
   * owns the physical device. The normalized device is created on the first
   * normalized pen packet rather than at session startup, so raw attachment
   * does not replace a temporary tablet in the X11 desktop.
   *
   * @param context Per-client virtual input context.
   * @param enabled Whether the normalized fallback should exist.
   */
  void set_pen_tablet_enabled(client_context_t &context, bool enabled);

  /**
   * @brief Return whether the normalized pen-tablet fallback exists.
   *
   * @param context Per-client virtual input context.
   * @return True when the normalized pen tablet is present.
   */
  bool pen_tablet_enabled(const client_context_t &context);

  /**
   * @brief Return the shared libvirtualhid context from a platform input backend.
   *
   * @param input Platform input backend.
   * @return Shared libvirtualhid input context.
   */
  input_context_t &get_input_context(input_t &input);

  /**
   * @brief Return the per-client libvirtualhid context from a platform input backend.
   *
   * @param input Per-client platform input backend.
   * @return Per-client libvirtualhid input context.
   */
  client_context_t &get_client_context(client_input_t *input);

  /**
   * @brief Create a platform-default libvirtualhid runtime.
   *
   * @param backend Backend used to create the runtime.
   * @return Runtime instance.
   */
  std::unique_ptr<lvh::Runtime> create_runtime(lvh::BackendKind backend = lvh::BackendKind::platform_default);

  /**
   * @brief Move the virtual mouse relatively.
   *
   * @param context Input context.
   * @param delta_x Horizontal delta.
   * @param delta_y Vertical delta.
   */
  void move_mouse(input_context_t &context, int delta_x, int delta_y);

  /**
   * @brief Move the virtual mouse absolutely inside a target touch port.
   *
   * @param context Input context.
   * @param touch_port Target coordinate space.
   * @param x Absolute X coordinate.
   * @param y Absolute Y coordinate.
   */
  void abs_mouse(input_context_t &context, const touch_port_t &touch_port, float x, float y);

  /**
   * @brief Submit a mouse button event.
   *
   * @param context Input context.
   * @param button Moonlight mouse button.
   * @param release Whether the button was released.
   */
  void button_mouse(input_context_t &context, int button, bool release);

  /**
   * @brief Submit vertical scroll input.
   *
   * @param context Input context.
   * @param high_res_distance High-resolution scroll distance.
   */
  void scroll(input_context_t &context, int high_res_distance);

  /**
   * @brief Submit horizontal scroll input.
   *
   * @param context Input context.
   * @param high_res_distance High-resolution scroll distance.
   */
  void hscroll(input_context_t &context, int high_res_distance);

  /**
   * @brief Submit a keyboard key transition.
   *
   * @param context Input context.
   * @param modcode Portable key code.
   * @param release Whether the key was released.
   * @param flags Bit flags that modify the requested operation.
   */
  void keyboard_update(input_context_t &context, std::uint16_t modcode, bool release, std::uint8_t flags);

  /**
   * @brief Submit UTF-8 text input.
   *
   * @param context Input context.
   * @param utf8 UTF-8 text buffer.
   * @param size Text buffer size.
   */
  void unicode(input_context_t &context, const char *utf8, int size);

  /**
   * @brief Submit a pen event.
   *
   * @param context Client context.
   * @param touch_port Touch coordinate bounds used for scaling.
   * @param pen Pen event.
   */
  void pen_update(client_context_t &context, const touch_port_t &touch_port, const pen_input_t &pen);

}  // namespace platf::virtualhid
