// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_GFX_SCREEN_H_
#define UI_GFX_SCREEN_H_

#include "base/basictypes.h"
#include "ui/base/ui_export.h"
#include "ui/gfx/display.h"
#if !defined(__LB_SHELL__)
#include "ui/gfx/native_widget_types.h"
#endif
#include "ui/gfx/point.h"
#if !defined(__LB_SHELL__)
#include "ui/gfx/screen_type_delegate.h"
#endif

namespace gfx {
class DisplayObserver;
class Rect;

// A utility class for getting various info about screen size, displays,
// cursor position, etc.
class UI_EXPORT Screen {
 public:
#if !defined(__LB_SHELL__)
  // Retrieves the Screen that the specified NativeView belongs to. A value of
  // NULL is treated as |SCREEN_TYPE_NATIVE|.
  static Screen* GetScreenFor(NativeView view);
#endif

  // Returns the SCREEN_TYPE_NATIVE Screen. This should be used with caution,
  // as it is likely to be incorrect for code that runs on Windows.
  static Screen* GetNativeScreen();

#if !defined(__LB_SHELL__)
  // Sets the global screen for a particular screen type. Only the _NATIVE
  // ScreenType must be provided.
  static void SetScreenInstance(ScreenType type, Screen* instance);

  // Returns the global screen for a particular type. Types other than _NATIVE
  // may be NULL.
  static Screen* GetScreenByType(ScreenType type);

  // Sets the global ScreenTypeDelegate. May be left unset if the platform
  // uses only the _NATIVE ScreenType.
  static void SetScreenTypeDelegate(ScreenTypeDelegate* delegate);
#endif

  Screen();
  virtual ~Screen();

  // Returns true if DIP is enabled.
  virtual bool IsDIPEnabled() = 0;

  // Returns the current absolute position of the mouse pointer.
  virtual gfx::Point GetCursorScreenPoint() = 0;

#if !defined(__LB_SHELL__)
  // Returns the window under the cursor.
  virtual gfx::NativeWindow GetWindowAtCursorScreenPoint() = 0;
#endif

  // Returns the number of displays.
  // Mirrored displays are excluded; this method is intended to return the
  // number of distinct, usable displays.
  virtual int GetNumDisplays() = 0;

#if !defined(__LB_SHELL__)
  // Returns the display nearest the specified window.
  virtual gfx::Display GetDisplayNearestWindow(NativeView view) const = 0;
#endif

  // Returns the the display nearest the specified point.
  virtual gfx::Display GetDisplayNearestPoint(
      const gfx::Point& point) const = 0;

  // Returns the display that most closely intersects the provided bounds.
  virtual gfx::Display GetDisplayMatching(
      const gfx::Rect& match_rect) const = 0;

  // Returns the primary display.
  virtual gfx::Display GetPrimaryDisplay() const = 0;

  // Adds/Removes display observers.
  virtual void AddObserver(DisplayObserver* observer) = 0;
  virtual void RemoveObserver(DisplayObserver* observer) = 0;

 private:
  DISALLOW_COPY_AND_ASSIGN(Screen);
};

Screen* CreateNativeScreen();

}  // namespace gfx

#endif  // UI_GFX_SCREEN_H_
