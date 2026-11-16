#include "test_main.h"

#include "core/clock.h"
#include "core/cover_controller.h"
#include "core/cover_overlay.h"

using htmlppt::CoverController;
using htmlppt::CoverOverlay;
using htmlppt::CoverTiming;
using htmlppt::Duration;
using htmlppt::FakeClock;

namespace {

class FakeOverlay final : public CoverOverlay {
 public:
  bool Show() override {
    show_count++;
    visible = true;
    return true;
  }
  void Hide() override {
    hide_count++;
    visible = false;
  }
  bool SetOpacity(double value) override {
    opacity = value;
    return true;
  }
  void ReassertTopMost() override { topmost_count++; }

  int show_count = 0;
  int hide_count = 0;
  int topmost_count = 0;
  bool visible = false;
  double opacity = 0.0;
};

} // namespace

HTMLPPT_TEST_CASE(cover_controller_drives_overlay) {
  FakeClock clock;
  FakeOverlay overlay;
  CoverController controller(
      CoverTiming{Duration{300}, Duration{1000}, Duration{10000}}, clock,
      overlay);

  HTMLPPT_CHECK(controller.FadeIn());
  HTMLPPT_CHECK_EQ(overlay.show_count, 1);
  HTMLPPT_CHECK_EQ(overlay.opacity, 0.0);

  clock.advance(Duration{150});
  auto tick = controller.Tick();
  HTMLPPT_CHECK_EQ(tick.opacity > 0.49 && tick.opacity < 0.51, true);
  HTMLPPT_CHECK_EQ(overlay.opacity > 0.49 && overlay.opacity < 0.51, true);

  clock.advance(Duration{150});
  tick = controller.Tick();
  HTMLPPT_CHECK(tick.hold_reached);
  HTMLPPT_CHECK_EQ(overlay.topmost_count, 1);

  controller.FadeOut();
  clock.advance(Duration{1300});
  tick = controller.Tick();
  HTMLPPT_CHECK(!tick.done);
  clock.advance(Duration{300});
  tick = controller.Tick();
  HTMLPPT_CHECK(tick.done);
  HTMLPPT_CHECK_EQ(overlay.hide_count, 1);
  HTMLPPT_CHECK(!overlay.visible);
}

HTMLPPT_TEST_CASE(cover_controller_hide_is_immediate) {
  FakeClock clock;
  FakeOverlay overlay;
  CoverController controller(
      CoverTiming{Duration{300}, Duration{1000}, Duration{10000}}, clock,
      overlay);
  controller.FadeIn();
  controller.Hide();
  HTMLPPT_CHECK_EQ(overlay.hide_count, 1);
  HTMLPPT_CHECK(!overlay.visible);
}
