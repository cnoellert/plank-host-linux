#include <cstring>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#ifdef __linux__
  #include <linux/input.h>
#endif

extern "C" {
#include <moonlight-common-c/src/plank.h>
}

#include "src/raw_hid_tablet.h"
#include "src/utility.h"

namespace {
  std::vector<std::uint8_t> make_frame(
    const PLANK_RAW_HID_MESSAGE_TYPE type,
    const std::uint16_t interface_id,
    const std::uint16_t generation,
    const void *payload,
    const std::size_t payload_size
  ) {
    std::vector<std::uint8_t> frame(sizeof(PLANK_RAW_HID_WIRE_HEADER) + payload_size);
    PLANK_RAW_HID_WIRE_HEADER header {};
    header.magic = util::endian::little(static_cast<std::uint32_t>(PLANK_RAW_HID_WIRE_MAGIC));
    header.version = util::endian::little(static_cast<std::uint16_t>(PLANK_RAW_HID_WIRE_VERSION));
    header.type = util::endian::little(static_cast<std::uint16_t>(type));
    header.interfaceId = util::endian::little(interface_id);
    header.generation = util::endian::little(generation);
    header.payloadLength = util::endian::little(static_cast<std::uint32_t>(payload_size));
    std::memcpy(frame.data(), &header, sizeof(header));
    if (payload_size != 0) {
      std::memcpy(frame.data() + sizeof(header), payload, payload_size);
    }
    return frame;
  }
}  // namespace

TEST(RawHidTablet, RejectsMalformedTransportFrames) {
  const auto mail = std::make_shared<safe::mail_raw_t>();
  raw_hid::tablet_t tablet {mail->queue<std::vector<std::uint8_t>>("raw-hid-test")};

  EXPECT_FALSE(tablet.handle({}));

  auto frame = make_frame(PLANK_RAW_HID_DETACH, 0, 1, nullptr, 0);
  reinterpret_cast<PLANK_RAW_HID_WIRE_HEADER *>(frame.data())->magic = 0;
  EXPECT_FALSE(tablet.handle(frame));

  frame = make_frame(PLANK_RAW_HID_DETACH, 0, 1, nullptr, 0);
  reinterpret_cast<PLANK_RAW_HID_WIRE_HEADER *>(frame.data())->payloadLength = util::endian::little<std::uint32_t>(1);
  EXPECT_FALSE(tablet.handle(frame));

  frame = make_frame(PLANK_RAW_HID_DETACH, 0, 1, nullptr, 0);
  reinterpret_cast<PLANK_RAW_HID_WIRE_HEADER *>(frame.data())->version =
    util::endian::little<std::uint16_t>(PLANK_RAW_HID_WIRE_VERSION - 1);
  EXPECT_FALSE(tablet.handle(frame));
}

TEST(RawHidTablet, EnforcesGenerationAndInterfaceBounds) {
  const auto mail = std::make_shared<safe::mail_raw_t>();
  raw_hid::tablet_t tablet {mail->queue<std::vector<std::uint8_t>>("raw-hid-test")};
  PLANK_RAW_HID_DEVICE_MESSAGE device {};
  device.interfaceCount = util::endian::little<std::uint16_t>(2);
  device.bus = util::endian::little<std::uint16_t>(3);
  device.vendor = util::endian::little<std::uint32_t>(0x056a);
  device.product = util::endian::little<std::uint32_t>(0x0357);

  EXPECT_TRUE(tablet.handle(make_frame(
    PLANK_RAW_HID_DEVICE,
    0,
    7,
    &device,
    sizeof(device)
  )));

  const std::uint8_t descriptor[] {0x05, 0x0d, 0x09, 0x02};
  EXPECT_FALSE(tablet.handle(make_frame(
    PLANK_RAW_HID_DESCRIPTOR,
    2,
    7,
    descriptor,
    sizeof(descriptor)
  )));
  EXPECT_FALSE(tablet.handle(make_frame(
    PLANK_RAW_HID_DESCRIPTOR,
    0,
    8,
    descriptor,
    sizeof(descriptor)
  )));
  EXPECT_TRUE(tablet.handle(make_frame(
    PLANK_RAW_HID_DESCRIPTOR,
    0,
    7,
    descriptor,
    sizeof(descriptor)
  )));
  EXPECT_FALSE(tablet.handle(make_frame(
    PLANK_RAW_HID_DESCRIPTOR,
    0,
    7,
    descriptor,
    sizeof(descriptor)
  )));
}

TEST(RawHidTablet, ReusesIdenticalEndpointsAcrossFocusAndTransportResume) {
  if (!raw_hid::available()) {
    GTEST_SKIP() << "/dev/uhid is unavailable to the test process";
  }

  const auto first_mail = std::make_shared<safe::mail_raw_t>();
  raw_hid::tablet_t tablet {first_mail->queue<std::vector<std::uint8_t>>("raw-hid-test")};
  PLANK_RAW_HID_DEVICE_MESSAGE device {};
  device.interfaceCount = util::endian::little<std::uint16_t>(1);
  device.bus = util::endian::little<std::uint16_t>(3);
  device.vendor = util::endian::little<std::uint32_t>(0x056a);
  device.product = util::endian::little<std::uint32_t>(0x0358);
  std::memcpy(device.name, "PLANK Wacom test", sizeof("PLANK Wacom test"));
  std::memcpy(device.unique, "stable-tablet", 13);

  // Minimal valid HID application collection. The reconnect assertion is
  // based on the synchronous UHID endpoint creation epoch, not udev timing.
  const std::uint8_t descriptor[] {
    0x05, 0x01,  // Usage Page (Generic Desktop)
    0x09, 0x02,  // Usage (Mouse)
    0xa1, 0x01,  // Collection (Application)
    0xc0,  // End Collection
  };

  ASSERT_TRUE(tablet.handle(make_frame(PLANK_RAW_HID_DEVICE, 0, 7, &device, sizeof(device))));
  ASSERT_TRUE(tablet.handle(make_frame(PLANK_RAW_HID_DESCRIPTOR, 0, 7, descriptor, sizeof(descriptor))));
  const auto initial_epoch = tablet.endpoint_epoch();
  ASSERT_GT(initial_epoch, 0);

  ASSERT_TRUE(tablet.handle(make_frame(PLANK_RAW_HID_SUSPEND, 0, 7, nullptr, 0)));
  EXPECT_FALSE(tablet.handle(make_frame(PLANK_RAW_HID_SUSPEND, 1, 7, nullptr, 0)));
  EXPECT_FALSE(tablet.handle(make_frame(PLANK_RAW_HID_SUSPEND, 0, 8, nullptr, 0)));
  auto malformed_suspend = make_frame(PLANK_RAW_HID_SUSPEND, 0, 7, nullptr, 0);
  reinterpret_cast<PLANK_RAW_HID_WIRE_HEADER *>(malformed_suspend.data())->transactionId =
    util::endian::little<std::uint32_t>(1);
  EXPECT_FALSE(tablet.handle(malformed_suspend));
  const std::uint8_t invalid_suspend_payload = 0;
  EXPECT_FALSE(tablet.handle(make_frame(
    PLANK_RAW_HID_SUSPEND,
    0,
    7,
    &invalid_suspend_payload,
    sizeof(invalid_suspend_payload)
  )));
  ASSERT_TRUE(tablet.handle(make_frame(PLANK_RAW_HID_DEVICE, 0, 8, &device, sizeof(device))));
  ASSERT_TRUE(tablet.handle(make_frame(PLANK_RAW_HID_DESCRIPTOR, 0, 8, descriptor, sizeof(descriptor))));

  EXPECT_EQ(tablet.active_generation(), 8);
  EXPECT_EQ(tablet.endpoint_epoch(), initial_epoch);

  tablet.suspend();
  const auto resumed_mail = std::make_shared<safe::mail_raw_t>();
  tablet.rebind(resumed_mail->queue<std::vector<std::uint8_t>>("raw-hid-test-resumed"));
  ASSERT_TRUE(tablet.handle(make_frame(PLANK_RAW_HID_DEVICE, 0, 9, &device, sizeof(device))));
  ASSERT_TRUE(tablet.handle(make_frame(PLANK_RAW_HID_DESCRIPTOR, 0, 9, descriptor, sizeof(descriptor))));

  EXPECT_EQ(tablet.active_generation(), 9);
  EXPECT_EQ(tablet.endpoint_epoch(), initial_epoch);

  device.product = util::endian::little<std::uint32_t>(0x0357);
  ASSERT_TRUE(tablet.handle(make_frame(PLANK_RAW_HID_DEVICE, 0, 10, &device, sizeof(device))));
  ASSERT_TRUE(tablet.handle(make_frame(PLANK_RAW_HID_DESCRIPTOR, 0, 10, descriptor, sizeof(descriptor))));
  EXPECT_GT(tablet.endpoint_epoch(), initial_epoch);
}

#ifdef __linux__
TEST(RawHidTablet, ReleasesPenContactWithoutLeavingProximity) {
  raw_hid::input_node_state_t pen;
  pen.held_keys = {BTN_TOOL_PEN, BTN_TOUCH, BTN_STYLUS};
  pen.pressure = 4096;

  const std::vector<raw_hid::release_event_t> expected {
    {EV_KEY, BTN_TOUCH, 0},
    {EV_KEY, BTN_STYLUS, 0},
    {EV_ABS, ABS_PRESSURE, 0},
    {EV_SYN, SYN_REPORT, 0},
  };
  EXPECT_EQ(raw_hid::plan_contact_release(pen), expected);
}

TEST(RawHidTablet, KeepsToolProximityOnPenAndPadNodes) {
  raw_hid::input_node_state_t tools;
  tools.held_keys = {
    BTN_TOOL_PEN,
    BTN_TOOL_RUBBER,
    BTN_TOOL_BRUSH,
    BTN_TOOL_PENCIL,
    BTN_TOOL_AIRBRUSH,
    BTN_TOOL_FINGER,
    BTN_TOOL_MOUSE,
    BTN_TOOL_LENS,
  };
  tools.pressure = 0;

  EXPECT_TRUE(raw_hid::plan_contact_release(tools).empty());
}

TEST(RawHidTablet, ReleasesHeldPadKeys) {
  raw_hid::input_node_state_t pad;
  pad.held_keys = {BTN_0, BTN_5, BTN_TOOL_FINGER};

  const std::vector<raw_hid::release_event_t> expected {
    {EV_KEY, BTN_0, 0},
    {EV_KEY, BTN_5, 0},
    {EV_SYN, SYN_REPORT, 0},
  };
  EXPECT_EQ(raw_hid::plan_contact_release(pad), expected);
}

TEST(RawHidTablet, EndsActiveTouchesAndRestoresCurrentSlot) {
  raw_hid::input_node_state_t touch;
  touch.mt_current_slot = 2;
  touch.mt_tracking_ids = {14, -1, 15};
  touch.held_keys = {BTN_TOUCH, BTN_TOOL_DOUBLETAP};

  const std::vector<raw_hid::release_event_t> expected {
    {EV_ABS, ABS_MT_SLOT, 0},
    {EV_ABS, ABS_MT_TRACKING_ID, -1},
    {EV_ABS, ABS_MT_SLOT, 2},
    {EV_ABS, ABS_MT_TRACKING_ID, -1},
    {EV_ABS, ABS_MT_SLOT, 2},
    {EV_KEY, BTN_TOUCH, 0},
    {EV_KEY, BTN_TOOL_DOUBLETAP, 0},
    {EV_SYN, SYN_REPORT, 0},
  };
  EXPECT_EQ(raw_hid::plan_contact_release(touch), expected);
}

TEST(RawHidTablet, ReleasesResidualPressureAlone) {
  raw_hid::input_node_state_t pen;
  pen.held_keys = {BTN_TOOL_PEN};
  pen.pressure = 12;

  const std::vector<raw_hid::release_event_t> expected {
    {EV_ABS, ABS_PRESSURE, 0},
    {EV_SYN, SYN_REPORT, 0},
  };
  EXPECT_EQ(raw_hid::plan_contact_release(pen), expected);
}

TEST(RawHidTablet, PlansNothingWithoutHeldContact) {
  raw_hid::input_node_state_t idle;
  idle.pressure = 0;
  idle.mt_current_slot = 0;
  idle.mt_tracking_ids = {-1, -1};

  EXPECT_TRUE(raw_hid::plan_contact_release(idle).empty());
  EXPECT_TRUE(raw_hid::plan_contact_release({}).empty());
}
#endif
