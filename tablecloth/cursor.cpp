#include "cursor.hpp"

#include <math.h>
#include <stdlib.h>

#include "util/logging.hpp"

#include "wlroots.hpp"

#include "desktop.hpp"
#include "input.hpp"
#include "seat.hpp"
#include "server.hpp"
#include "view.hpp"
#include "xcursor.hpp"

namespace cloth {

  auto Cursor::handle_tablet_tool_position(Tablet& tablet,
                                           wlr::tablet_tool_t* wlr_tool,
                                           bool change_x,
                                           bool change_y,
                                           double x,
                                           double y,
                                           double dx,
                                           double dy,
                                           unsigned time) -> void
  {
    if (!change_x && !change_y) {
      return;
    }
    switch (wlr_tool->type) {
    case WLR_TABLET_TOOL_TYPE_MOUSE:
      // They are 0 either way when they weren't modified
      wlr_cursor_move(wlr_cursor, &tablet.wlr_device, dx, dy);
      break;
    default:
      wlr_cursor_warp_absolute(wlr_cursor, &tablet.wlr_device, change_x ? x : NAN,
                               change_y ? y : NAN);
    }
    double sx, sy;
    View* view = nullptr;
    Desktop& desktop = seat.input.server.desktop;
    wlr::surface_t* surface = desktop.surface_at(wlr_cursor->x, wlr_cursor->y, sx, sy, view);
    auto& tool = *(TabletTool*) wlr_tool->data;
    if (!surface) {
      wlr_tablet_v2_tablet_tool_notify_proximity_out(&tool.tablet_v2_tool);
      if (!tool.in_fallback_mode) cloth_debug("No surface found, Using tablet tool in fallback mode");
      tool.in_fallback_mode = true;
      update_position(time);
      return;
    }
    if (!wlr_surface_accepts_tablet_v2(&tablet.tablet_v2, surface)) {
      wlr_tablet_v2_tablet_tool_notify_proximity_out(&tool.tablet_v2_tool);
      if (!tool.in_fallback_mode)
        cloth_debug("Surface does not accept tablet, using tool in fallback mode");
      update_position(time);
      tool.in_fallback_mode = true;
      return;
    }
    if (tool.in_fallback_mode) {
      cloth_debug("Switching tablet tool back to native mode");
      mode = Mode::Passthrough;
      tool.in_fallback_mode = false;
    }
    wlr_tablet_v2_tablet_tool_notify_proximity_in(&tool.tablet_v2_tool, &tablet.tablet_v2, surface);
    wlr_tablet_v2_tablet_tool_notify_motion(&tool.tablet_v2_tool, sx, sy);
  }


  Cursor::Cursor(Seat& p_seat, wlr::cursor_t* p_cursor) noexcept
    : seat(p_seat), wlr_cursor(p_cursor), default_xcursor(xcursor_default)
  {
    Desktop& desktop = seat.input.server.desktop;
    wlr_cursor_attach_output_layout(wlr_cursor, desktop.layout);

    // add input signals
    on_motion.add_to(wlr_cursor->events.motion);
    on_motion = [this](void* data) {
      wlr_idle_notify_activity(seat.input.server.desktop.idle, seat.wlr_seat);
      set_visible(true);
      auto* event = (wlr::event_pointer_motion_t*) data;
      double dx = event->delta_x;
      double dy = event->delta_y;
      if (active_constraint) {
        auto& view = pointer_view->view;
        // TODO: handle rotated views
        if (view.rotation == 0.0) {
          double lx1 = wlr_cursor->x;
          double ly1 = wlr_cursor->y;
          double lx2 = lx1 + dx;
          double ly2 = ly1 + dy;
          double sx1 = lx1 - view.x;
          double sy1 = ly1 - view.y;
          double sx2 = lx2 - view.x;
          double sy2 = ly2 - view.y;
          double sx2_confined, sy2_confined;
          if (!wlr_region_confine(&confine, sx1, sy1, sx2, sy2, &sx2_confined, &sy2_confined)) {
            return;
          }
          dx = sx2_confined - sx1;
          dy = sy2_confined - sy1;
        }
      }
      wlr_cursor_move(wlr_cursor, event->device, dx, dy);

      update_position(event->time_msec);
    };

    on_motion_absolute.add_to(wlr_cursor->events.motion_absolute);
    on_motion_absolute = [this](void* data) {
      wlr_idle_notify_activity(seat.input.server.desktop.idle, seat.wlr_seat);
      set_visible(true);
      auto* event = (wlr::event_pointer_motion_absolute_t*) data;

      double lx, ly;
      wlr_cursor_absolute_to_layout_coords(wlr_cursor, event->device, event->x, event->y, &lx, &ly);
      if (pointer_view) {
        auto& view = pointer_view->view;
        if (active_constraint && !pixman_region32_contains_point(&confine, floor(lx - view.x),
                                                                 floor(ly - view.y), NULL)) {
          return;
        }
      }
      wlr_cursor_warp_closest(wlr_cursor, event->device, lx, ly);

      update_position(event->time_msec);
    };

    on_button.add_to(wlr_cursor->events.button);
    on_button = [this](void* data) {
      wlr_idle_notify_activity(seat.input.server.desktop.idle, seat.wlr_seat);
      set_visible(true);
      auto* event = (wlr::event_pointer_button_t*) data;
      press_button(*event->device, event->time_msec, wlr::Button(event->button), event->state,
                   wlr_cursor->x, wlr_cursor->y);
    };

    on_axis.add_to(wlr_cursor->events.axis);
    on_axis = [this](void* data) {
      wlr_idle_notify_activity(seat.input.server.desktop.idle, seat.wlr_seat);
      set_visible(true);
      auto* event = (wlr::event_pointer_axis_t*) data;
      wlr_seat_pointer_notify_axis(this->seat.wlr_seat, event->time_msec, event->orientation,
                                   event->delta, event->delta_discrete, event->source);
    };

    on_touch_down.add_to(wlr_cursor->events.touch_down);
    on_touch_down = [this](void* data) {
      wlr_idle_notify_activity(seat.input.server.desktop.idle, seat.wlr_seat);
      auto* event = (wlr::event_touch_down_t*) data;
      Desktop& desktop = seat.input.server.desktop;
      double lx, ly;
      wlr_cursor_absolute_to_layout_coords(wlr_cursor, event->device, event->x, event->y, &lx, &ly);
      _is_visible = true;
      set_visible(false);

      double sx, sy;
      View* v;
      auto* surface = desktop.surface_at(lx, ly, sx, sy, v);

      if (wlr_seat_touch_num_points(this->seat.wlr_seat) == 0 && !current_gesture) {
        auto* output = desktop.output_at(lx, ly);
        if (output) {
          current_gesture =
            TouchGesture::create(event->touch_id, {lx, ly},
                                 {output->wlr_output.lx, output->wlr_output.ly,
                                  output->wlr_output.width, output->wlr_output.height});
          if (current_gesture)
            cloth_debug("Gesture possibly begun: {}", util::enum_cast(current_gesture.value().side));
        }
      }

      uint32_t serial = 0;
      if (surface && seat.allow_input(*surface->resource)) {
        serial = wlr_seat_touch_notify_down(this->seat.wlr_seat, surface, event->time_msec,
                                            event->touch_id, sx, sy);
      }

      if (serial && wlr_seat_touch_num_points(this->seat.wlr_seat) == 1) {
        seat.touch_id = event->touch_id;
        seat.touch_x = lx;
        seat.touch_y = ly;
        press_button(*event->device, event->time_msec, wlr::Button::left, WLR_BUTTON_PRESSED, lx,
                     ly);
      }
      if (serial && wlr_seat_touch_num_points(this->seat.wlr_seat) == 2) {
        seat.touch_id = event->touch_id;
        seat.touch_x = lx;
        seat.touch_y = ly;
        press_button(*event->device, event->time_msec, wlr::Button::right, WLR_BUTTON_PRESSED, lx,
                     ly);
      }
    };

    on_touch_up.add_to(wlr_cursor->events.touch_up);
    on_touch_up = [this](void* data) {
      wlr_idle_notify_activity(seat.input.server.desktop.idle, seat.wlr_seat);
      auto* event = (wlr::event_touch_up_t*) data;
      wlr::touch_point_t* point = wlr_seat_touch_get_point(this->seat.wlr_seat, event->touch_id);

      if (current_gesture) {
        bool valid = current_gesture.value().on_touch_up({seat.touch_x, seat.touch_y});
        if (valid) {
          cloth_debug("SlideGesture detected: {}", util::enum_cast(current_gesture.value().side));
          switch (current_gesture.value().side) {
          case Side::top:
            seat.input.server.desktop.run_command("exec killall cloth-bar || cloth-bar");
            break;
          case Side::bottom:
            seat.input.server.desktop.run_command("exec killall cloth-kbd || cloth-kbd");
            break;
          case Side::left: seat.input.server.desktop.run_command("switch_workspace prev"); break;
          case Side::right: seat.input.server.desktop.run_command("switch_workspace next"); break;
          default: break;
          }
        } else {
          cloth_debug("Gesture cancelled");
        }
        current_gesture = std::nullopt;
      }

      if (!point) {
        return;
      }

      if (wlr_seat_touch_num_points(this->seat.wlr_seat) == 1) {
        press_button(*event->device, event->time_msec, wlr::Button::left, WLR_BUTTON_RELEASED,
                     seat.touch_x, seat.touch_y);
      }

      if (wlr_seat_touch_num_points(this->seat.wlr_seat) == 2) {
        press_button(*event->device, event->time_msec, wlr::Button::right, WLR_BUTTON_RELEASED,
                     seat.touch_x, seat.touch_y);
      }

      wlr_seat_touch_notify_up(this->seat.wlr_seat, event->time_msec, event->touch_id);
    };

    on_touch_motion.add_to(wlr_cursor->events.touch_motion);
    on_touch_motion = [this](void* data) {
      wlr_idle_notify_activity(seat.input.server.desktop.idle, seat.wlr_seat);
      auto* event = (wlr::event_touch_motion_t*) data;
      auto& desktop = seat.input.server.desktop;
      wlr::touch_point_t* point = wlr_seat_touch_get_point(this->seat.wlr_seat, event->touch_id);
      if (!point) {
        return;
      }

      double lx, ly;
      wlr_cursor_absolute_to_layout_coords(wlr_cursor, event->device, event->x, event->y, &lx, &ly);

      double sx, sy;
      View* view;
      wlr::surface_t* surface = desktop.surface_at(lx, ly, sx, sy, view);

      if (surface && seat.allow_input(*surface->resource)) {
        wlr_seat_touch_point_focus(this->seat.wlr_seat, surface, event->time_msec, event->touch_id,
                                   sx, sy);
        wlr_seat_touch_notify_motion(this->seat.wlr_seat, event->time_msec, event->touch_id, sx,
                                     sy);
      } else {
        wlr_seat_touch_point_clear_focus(this->seat.wlr_seat, event->time_msec, event->touch_id);
      }

      if (event->touch_id == seat.touch_id) {
        seat.touch_x = lx;
        seat.touch_y = ly;
      }
    };

    on_tool_axis.add_to(wlr_cursor->events.tablet_tool_axis);
    on_tool_axis = [this](void* data) {
      wlr_idle_notify_activity(seat.input.server.desktop.idle, seat.wlr_seat);
      set_visible(true);
      auto* event = (wlr::event_tablet_tool_axis_t*) data;
      assert(event->tool->data);
      auto& tool = *(TabletTool*) event->tool->data;
      auto& tablet = *(Tablet*) event->device->data;

      /**
       * We need to handle them ourselves, not pass it into the cursor
       * without any consideration
       */

      // TODO: handle cursor constraints for tools too.
      handle_tablet_tool_position(tablet, event->tool, event->updated_axes & WLR_TABLET_TOOL_AXIS_X,
                                  event->updated_axes & WLR_TABLET_TOOL_AXIS_Y, event->x, event->y,
                                  event->dx, event->dy, event->time_msec);

      if (event->updated_axes & WLR_TABLET_TOOL_AXIS_PRESSURE) {
        wlr_tablet_v2_tablet_tool_notify_pressure(&tool.tablet_v2_tool, event->pressure);
      }
      if (event->updated_axes & WLR_TABLET_TOOL_AXIS_DISTANCE) {
        wlr_tablet_v2_tablet_tool_notify_distance(&tool.tablet_v2_tool, event->distance);
      }
      if (event->updated_axes & WLR_TABLET_TOOL_AXIS_TILT_X) {
        tool.tilt_x = event->tilt_x;
      }
      if (event->updated_axes & WLR_TABLET_TOOL_AXIS_TILT_Y) {
        tool.tilt_y = event->tilt_y;
      }
      if (event->updated_axes & (WLR_TABLET_TOOL_AXIS_TILT_X | WLR_TABLET_TOOL_AXIS_TILT_Y)) {
        wlr_tablet_v2_tablet_tool_notify_tilt(&tool.tablet_v2_tool, tool.tilt_x, tool.tilt_y);
      }
      if (event->updated_axes & WLR_TABLET_TOOL_AXIS_ROTATION) {
        wlr_tablet_v2_tablet_tool_notify_rotation(&tool.tablet_v2_tool, event->rotation);
      }
      if (event->updated_axes & WLR_TABLET_TOOL_AXIS_SLIDER) {
        wlr_tablet_v2_tablet_tool_notify_slider(&tool.tablet_v2_tool, event->slider);
      }
      if (event->updated_axes & WLR_TABLET_TOOL_AXIS_WHEEL) {
        wlr_tablet_v2_tablet_tool_notify_wheel(&tool.tablet_v2_tool, event->wheel_delta, 0);
      }
    };

    on_tool_tip.add_to(wlr_cursor->events.tablet_tool_tip);
    on_tool_tip = [this](void* data) {
      wlr_idle_notify_activity(seat.input.server.desktop.idle, seat.wlr_seat);
      set_visible(true);
      auto* event = (wlr::event_tablet_tool_tip_t*) data;
      auto& tool = *(TabletTool*) event->tool->data;

      auto button =
        event->tool->type == WLR_TABLET_TOOL_TYPE_ERASER ? wlr::Button::right : wlr::Button::left;

      if (event->state == WLR_TABLET_TOOL_TIP_DOWN) {
        if (tool.in_fallback_mode) {
          press_button(*event->device, event->time_msec, button, WLR_BUTTON_PRESSED, event->x,
                       event->y);
        } else {
          wlr_tablet_v2_tablet_tool_notify_down(&tool.tablet_v2_tool);
          wlr_tablet_tool_v2_start_implicit_grab(&tool.tablet_v2_tool);
        }
      } else {
        if (tool.in_fallback_mode) {
          press_button(*event->device, event->time_msec, button, WLR_BUTTON_RELEASED, event->x,
                       event->y);
        } else {
          wlr_tablet_v2_tablet_tool_notify_up(&tool.tablet_v2_tool);
        }
      }
    };

    on_tool_proximity.add_to(wlr_cursor->events.tablet_tool_proximity);
    on_tool_proximity = [this](void* data) {
      Desktop& desktop = seat.input.server.desktop;
      wlr_idle_notify_activity(seat.input.server.desktop.idle, seat.wlr_seat);
      set_visible(true);
      auto* event = (wlr::event_tablet_tool_proximity_t*) data;
      wlr::tablet_tool_t* wlr_tool = event->tool;
      if (!wlr_tool->data) {
        // This is attached to wlr_tool.data, and deleted when wlr_tool is destroyed
        // TODO: cleaner solution? I mean, this works fine...
        new TabletTool(seat, *wlr_tablet_tool_create(desktop.tablet_v2, seat.wlr_seat, wlr_tool));
      }
      if (event->state == WLR_TABLET_TOOL_PROXIMITY_IN) {
        handle_tablet_tool_position(*(Tablet*) event->device->data, event->tool, true, true,
                                    event->x, event->y, 0, 0, event->time_msec);
      }
      if (event->state == WLR_TABLET_TOOL_PROXIMITY_OUT) {
        auto& tool = *(TabletTool*) wlr_tool->data;
        wlr_tablet_v2_tablet_tool_notify_proximity_out(&tool.tablet_v2_tool);
        return;
      }
    };


    on_tool_button.add_to(wlr_cursor->events.tablet_tool_button);
    on_tool_button = [this](void* data) {
      wlr_idle_notify_activity(seat.input.server.desktop.idle, seat.wlr_seat);
      set_visible(true);
      auto* event = (wlr::event_tablet_tool_button_t*) data;
      auto& tool = *(TabletTool*) event->tool->data;

      wlr_tablet_v2_tablet_tool_notify_button(&tool.tablet_v2_tool,
                                              (enum zwp_tablet_pad_v2_button_state) event->button,
                                              (enum zwp_tablet_pad_v2_button_state) event->state);
    };

    on_request_set_cursor.add_to(seat.wlr_seat->events.request_set_cursor);
    on_request_set_cursor = [this](void* data) {
      wlr_idle_notify_activity(seat.input.server.desktop.idle, seat.wlr_seat);
      if (!_is_visible) return;
      auto* event = (wlr::seat_pointer_request_set_cursor_event_t*) data;
      wlr::surface_t* focused_surface = event->seat_client->seat->pointer_state.focused_surface;
      bool has_focused = focused_surface != nullptr && focused_surface->resource != nullptr;
      struct wl_client* focused_client = nullptr;
      if (has_focused) {
        focused_client = wl_resource_get_client(focused_surface->resource);
      }
      if (event->seat_client->client != focused_client || mode != Mode::Passthrough) {
        cloth_debug("Denying request to set cursor from unfocused client");
        return;
      }

      wlr_cursor_set_surface(wlr_cursor, event->surface, event->hotspot_x, event->hotspot_y);
      cursor_client = event->seat_client->client;
    };

    on_focus_change.add_to(seat.wlr_seat->pointer_state.events.focus_change);
    on_focus_change = [this](void* data) {
      auto* event = (wlr::seat_pointer_focus_change_event_t*) data;
      double sx = event->sx;
      double sy = event->sy;
      double lx = wlr_cursor->x;
      double ly = wlr_cursor->y;
      cloth_debug("entered surface {}, lx: {}, ly: {}, sx: {}, sy: {}", (void*) event->new_surface, lx, ly, sx,
           sy);
      constrain(wlr_pointer_constraints_v1_constraint_for_surface(
                  seat.input.server.desktop.pointer_constraints, event->new_surface, seat.wlr_seat),
                sx, sy);
    };

    // Added in ::constrain()
    on_constraint_commit = [this](void* data) {
      assert(active_constraint->surface == data);
      auto& desktop = seat.input.server.desktop;
      View* view;
      double sx, sy;
      auto* surface = desktop.surface_at(wlr_cursor->x, wlr_cursor->y, sx, sy, view);
      // This should never happen but views move around right when they're
      // created from (0, 0) to their actual coordinates.
      if (surface != active_constraint->surface) {
        update_focus();
      } else {
        constrain(active_constraint, sx, sy);
      }
    };
  }

  Cursor::~Cursor() noexcept
  {
    // TODO
  }

  auto Cursor::set_visible(bool vis) -> void
  {
    if (vis == _is_visible) return;
    if (vis) {
      if (wlr_cursor)
        wlr_xcursor_manager_set_cursor_image(xcursor_manager, default_xcursor.c_str(), wlr_cursor);
    } else {
      if (wlr_cursor) wlr_cursor_set_image(wlr_cursor, nullptr, 0, 0, 0, 0, 0, 0);
    }
    _is_visible = vis;
  }

  void Cursor::passthrough_cursor(int64_t time)
  {
    double sx, sy;
    View* view = nullptr;
    auto* surface =
      seat.input.server.desktop.surface_at(wlr_cursor->x, wlr_cursor->y, sx, sy, view);
    wl::client_t* client = nullptr;
    if (surface) {
      client = wl_resource_get_client(surface->resource);
    }
    if (surface && !seat.allow_input(*surface->resource)) {
      cloth_debug("Input disallowed for surface");
      return;
    }

    if (cursor_client != client) {
      if (_is_visible)
        wlr_xcursor_manager_set_cursor_image(xcursor_manager, default_xcursor.c_str(), wlr_cursor);
      cursor_client = client;
    }

    if (view) {
      SeatView& seat_view = seat.seat_view_from_view(*view);
      if (pointer_view && !wlr_surface && (surface || &seat_view != pointer_view)) {
        pointer_view->deco_leave();
      }
      pointer_view = &seat_view;
      if (!surface) {
        pointer_view = &seat_view;
        seat_view.deco_motion(sx, sy);
      }
    } else {
      pointer_view = nullptr;
    }

    wlr_surface = surface;

    if (surface) {
      // https://github.com/swaywm/wlroots/commit/2e6eb097b6e23b8923bbfc68b1843d5ccde1955b
      // Whenever a new surface is created, we have to update the cursor focus,
      // even if there's no input event. So, we generate one motion event, and
      // reuse the code to update the proper cursor focus. We need to do this
      // for all surface roles - toplevels, popups, subsurfaces.
      bool focus_changed = (seat.wlr_seat->pointer_state.focused_surface != surface);
      wlr_seat_pointer_notify_enter(seat.wlr_seat, surface, sx, sy);
      if (!focus_changed && time > 0) {
        wlr_seat_pointer_notify_motion(seat.wlr_seat, time, sx, sy);
      }
    } else {
      wlr_seat_pointer_clear_focus(seat.wlr_seat);
    }

    for (auto& icon : seat.drag_icons) icon.update_position();
  }

  void Cursor::update_focus()
  {
    passthrough_cursor(-1);
  }

  void Cursor::update_position(uint32_t time)
  {
    View* view;
    switch (mode) {
    case Mode::Passthrough: passthrough_cursor(time); break;
    case Mode::Move:
      view = seat.get_focus();
      if (view != nullptr) {
        double dx = wlr_cursor->x - this->offs_x;
        double dy = wlr_cursor->y - this->offs_y;
        view->move(this->view_x + dx, this->view_y + dy);
      }
      break;
    case Mode::Resize:
      view = seat.get_focus();
      if (view != nullptr) {
        double dx = wlr_cursor->x - this->offs_x;
        double dy = wlr_cursor->y - this->offs_y;
        double x = view->x;
        double y = view->y;
        int width = this->view_width;
        int height = this->view_height;
        if (this->resize_edges & WLR_EDGE_TOP) {
          y = this->view_y + dy;
          height -= dy;
          if (height < 1) {
            y += height;
          }
        } else if (this->resize_edges & WLR_EDGE_BOTTOM) {
          height += dy;
        }
        if (this->resize_edges & WLR_EDGE_LEFT) {
          x = this->view_x + dx;
          width -= dx;
          if (width < 1) {
            x += width;
          }
        } else if (this->resize_edges & WLR_EDGE_RIGHT) {
          width += dx;
        }
        view->move_resize(x, y, width < 1 ? 1 : width, height < 1 ? 1 : height);
      }
      break;
    case Mode::Rotate:
      view = seat.get_focus();
      if (view != nullptr) {
        int ox = view->x + view->wlr_surface->current.width / 2,
            oy = view->y + view->wlr_surface->current.height / 2;
        int ux = this->offs_x - ox, uy = this->offs_y - oy;
        int vx = wlr_cursor->x - ox, vy = wlr_cursor->y - oy;
        float angle = atan2(ux * vy - uy * vx, vx * ux + vy * uy);
        int steps = 12;
        angle = round(angle / M_PI * steps) / (steps / M_PI);
        view->rotate(this->view_rotation + angle);
      }
      break;
    }
  }

  void Cursor::press_button(wlr::input_device_t& device,
                            uint32_t time,
                            wlr::Button button,
                            wlr::button_state_t state,
                            double lx,
                            double ly)
  {
    auto& desktop = seat.input.server.desktop;

    bool is_touch = device.type == WLR_INPUT_DEVICE_TOUCH;

    double sx, sy;
    View* view;
    wlr::surface_t* surface = desktop.surface_at(lx, ly, sx, sy, view);

    if (!is_touch) {
      cloth_debug("Pressed button");
      wlr_seat_pointer_notify_button(seat.wlr_seat, time, util::enum_cast(button), state);
    }

    if (state == WLR_BUTTON_PRESSED && view && seat.has_meta_pressed()) {
      view->workspace->set_focused_view(view);

      wlr::edges_t edges;
      switch (button) {
      case wlr::Button::left: seat.begin_move(*view); break;
      case wlr::Button::right:
        edges = WLR_EDGE_NONE;
        if (sx < view->wlr_surface->current.width / 2) {
          edges |= WLR_EDGE_LEFT;
        } else {
          edges |= WLR_EDGE_RIGHT;
        }
        if (sy < view->wlr_surface->current.height / 2) {
          edges |= WLR_EDGE_TOP;
        } else {
          edges |= WLR_EDGE_BOTTOM;
        }
        seat.begin_resize(*view, edges);
        break;
      case wlr::Button::middle: seat.begin_rotate(*view); break;
      default: break;
      }
    } else {
      if (view && !surface && pointer_view) {
        pointer_view->deco_button(sx, sy, button, state);
      }

      if (state == WLR_BUTTON_RELEASED && mode != Cursor::Mode::Passthrough) {
        mode = Mode::Passthrough;
      }

      switch (state) {
      case WLR_BUTTON_RELEASED:
        if (!is_touch) {
          update_position(time);
        }
        break;
      case WLR_BUTTON_PRESSED:
        if (view && surface == view->wlr_surface) {
          view->workspace->set_focused_view(view);
        }
        if (surface && wlr_surface_is_layer_surface(surface)) {
          auto* layer = wlr_layer_surface_v1_from_wlr_surface(surface);
          if (layer->current.keyboard_interactive) {
            seat.set_focus_layer(layer);
          }
        }
        break;
      }
    }

  }

  void Cursor::constrain(wlr::pointer_constraint_v1_t* constraint, double sx, double sy)
  {
    if (active_constraint == constraint) {
      return;
    }
    cloth_debug("roots_cursor_constrain({}, {})", (void*) this, (void*) constraint);
    cloth_debug("cursor->active_constraint: {}", (void*) active_constraint);
    on_constraint_commit.remove();
    if (active_constraint) {
      wlr_pointer_constraint_v1_send_deactivated(active_constraint);
    }
    active_constraint = constraint;
    if (constraint == nullptr) {
      return;
    }
    wlr_pointer_constraint_v1_send_activated(constraint);
    on_constraint_commit.add_to(constraint->surface->events.commit);
    pixman_region32_clear(&confine);
    pixman_region32_t* region = &constraint->region;
    if (!pixman_region32_contains_point(region, floor(sx), floor(sy), NULL)) {
      // Warp into region if possible
      int nboxes;
      pixman_box32_t* boxes = pixman_region32_rectangles(region, &nboxes);
      if (nboxes > 0) {
        auto& view = pointer_view->view;
        double sx = (boxes[0].x1 + boxes[0].x2) / 2.;
        double sy = (boxes[0].y1 + boxes[0].y2) / 2.;
        // TODO: rotate_child_position(&sx, &sy, 0, 0, view.width, view.height, view.rotation);
        double lx = view.x + sx;
        double ly = view.y + sy;
        wlr_cursor_warp_closest(wlr_cursor, NULL, lx, ly);
      }
    }
    // A locked pointer will result in an empty region, thus disallowing all movement
    if (constraint->type == WLR_POINTER_CONSTRAINT_V1_CONFINED) {
      pixman_region32_copy(&confine, region);
    }
  }
} // namespace cloth
