#include "seat.hpp"

#include "cursor.hpp"
#include "input.hpp"
#include "keyboard.hpp"
#include "seat.hpp"
#include "server.hpp"
#include "xcursor.hpp"

#include "util/algorithm.hpp"
#include "util/exception.hpp"
#include "util/logging.hpp"

namespace cloth {

  template<wl::output_transform_t T>
  constexpr float transform_matrix[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

  template<>
  constexpr float transform_matrix<WL_OUTPUT_TRANSFORM_NORMAL>[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  template<>
  constexpr float transform_matrix<WL_OUTPUT_TRANSFORM_90>[9] = {0, 1, 0, -1, 0, 1, 0, 0, 1};
  template<>
  constexpr float transform_matrix<WL_OUTPUT_TRANSFORM_180>[9] = {-1, 0, 1, 0, -1, 1, 0, 0, 1};
  template<>
  constexpr float transform_matrix<WL_OUTPUT_TRANSFORM_270>[9] = {0, -1, 1, 1, 0, 0, 0, 0, 1};

  constexpr auto get_transform_matrix(wl::output_transform_t t) -> const float*
  {
    switch (t) {
    case WL_OUTPUT_TRANSFORM_NORMAL: return transform_matrix<WL_OUTPUT_TRANSFORM_NORMAL>;
    case WL_OUTPUT_TRANSFORM_90: return transform_matrix<WL_OUTPUT_TRANSFORM_90>;
    case WL_OUTPUT_TRANSFORM_180: return transform_matrix<WL_OUTPUT_TRANSFORM_180>;
    case WL_OUTPUT_TRANSFORM_270: return transform_matrix<WL_OUTPUT_TRANSFORM_270>;
    default: return transform_matrix<WL_OUTPUT_TRANSFORM_NORMAL>;
    }
  }

  Seat::Seat(Input& input, const std::string& name)
    : wlr_seat(wlr_seat_create(input.server.wl_display, name.c_str())),
      input(input),
      cursor(*this, wlr_cursor_create())
  {
    if (!wlr_seat) throw util::exception("Could not create wlr_seat from name {}", name);
    wlr_seat->data = this;

    init_cursor();

    on_new_drag_icon = [this](void* data) { handle_new_drag_icon(data); };
    on_new_drag_icon.add_to(wlr_seat->events.new_drag_icon);

    on_destroy = [this] { util::erase_this(this->input.seats, *this); };
    on_destroy.add_to(wlr_seat->events.destroy);
  }

  Seat::~Seat()
  {
    wlr_seat_destroy(wlr_seat);
  }

  void Seat::reset_device_mappings(Device& device) noexcept
  {
    wlr::cursor_t* cursor = this->cursor.wlr_cursor;
    Config& config = input.config;

    wlr_cursor_map_input_to_output(cursor, &device.wlr_device, nullptr);
    device.on_output_transform.remove();
    Config::Device* dconfig;
    if ((dconfig = config.get_device(device.wlr_device))) {
      // TODO wlr_cursor_map_input_to_region(cursor, &device, &dconfig->mapped_box);
    }
  }

  void Seat::set_device_output_mappings(Device& device, wlr::output_t* output) noexcept
  {
    Config& config = input.config;
    Config::Device* dconfig = config.get_device(device.wlr_device);

    std::string_view mapped_output = "";
    if (dconfig != nullptr) {
      mapped_output = dconfig->mapped_output;
    }
    if (mapped_output.empty()) {
      mapped_output = device.wlr_device.output_name;
    }

    if (mapped_output == output->name) {
      cloth_debug("Input device {} mapped to output {}", device.wlr_device.name, output->name);
      // TODO: wlr_cursor_map_input_to_output(cursor, &device, output);
      device.on_output_transform.add_to(output->events.transform);
      device.on_output_transform = [&device, output](void* data) {
        cloth_debug("Output transform for device {}. Libinput: {}", device.wlr_device.name,
                    wlr_input_device_is_libinput(&device.wlr_device));
        if (wlr_input_device_is_libinput(&device.wlr_device)) {
          auto* libinput_handle = wlr_libinput_get_device_handle(&device.wlr_device);
          // libinput_device_config_calibration_set_matrix(libinput_handle,
          // get_transform_matrix(output->transform));
          libinput_device_config_rotation_set_angle(libinput_handle, [&] {
            switch (output->transform) {
            case WL_OUTPUT_TRANSFORM_NORMAL: return 0;
            case WL_OUTPUT_TRANSFORM_90: return 90;
            case WL_OUTPUT_TRANSFORM_180: return 180;
            case WL_OUTPUT_TRANSFORM_270: return 270;
            default: return 0;
            }
          }());
        }
      };
    }
  }

  void Seat::configure_cursor()
  {
    Config& config = input.config;
    Desktop& desktop = input.server.desktop;
    wlr::cursor_t* cursor = this->cursor.wlr_cursor;

    // reset mappings
    wlr_cursor_map_to_output(cursor, nullptr);
    for (auto& pointer : pointers) {
      reset_device_mappings(pointer);
    }
    for (auto& touch : this->touch) {
      reset_device_mappings(touch);
    }
    for (auto& tablet : tablets) {
      reset_device_mappings(tablet);
    }

    // configure device to output mappings
    std::string_view mapped_output = "";
    Config::Cursor* cc = config.get_cursor(wlr_seat->name);
    if (cc != nullptr) {
      mapped_output = cc->mapped_output;
    }
    for (auto& output : desktop.outputs) {
      if (mapped_output == output.wlr_output.name) {
        wlr_cursor_map_to_output(cursor, &output.wlr_output);
      }

      for (auto& pointer : pointers) {
        set_device_output_mappings(pointer, &output.wlr_output);
      }
      for (auto& tablet : tablets) {
        set_device_output_mappings(tablet, &output.wlr_output);
      }
      for (auto& touch : this->touch) {
        set_device_output_mappings(touch, &output.wlr_output);
      }
    }
  }

  void Seat::init_cursor()
  {
    configure_cursor();
    configure_xcursor();
  }

  void Seat::handle_new_drag_icon(void* data)
  {
    auto& wlr_drag_icon = *(wlr::drag_icon_t*) data;

    drag_icons.emplace_back(*this, wlr_drag_icon);
  }

  DragIcon::DragIcon(Seat& seat, wlr::drag_icon_t& wlr_icon) noexcept
    : seat(seat), wlr_drag_icon(wlr_icon)
  {
    on_surface_commit = [this] { update_position(); };
    on_surface_commit.add_to(wlr_drag_icon.surface->events.commit);

    auto handle_damage_whole = [this] { damage_whole(); };

    on_unmap = handle_damage_whole;
    on_unmap.add_to(wlr_drag_icon.events.unmap);
    on_map = handle_damage_whole;
    on_map.add_to(wlr_drag_icon.events.map);
    on_destroy = [this] {
      auto keep_alive = util::erase_this(this->seat.drag_icons, this);
      keep_alive->damage_whole();
    };
    on_destroy.add_to(wlr_drag_icon.events.destroy);

    update_position();
  }

  void DragIcon::update_position()
  {
    damage_whole();

    wlr::drag_icon_t& wlr_icon = wlr_drag_icon;
    wlr::cursor_t* cursor = seat.cursor.wlr_cursor;
    if (wlr_icon.is_pointer) {
      x = cursor->x;
      y = cursor->y;
    } else {
      wlr::touch_point_t* point = wlr_seat_touch_get_point(seat.wlr_seat, wlr_icon.touch_id);
      if (point == nullptr) {
        return;
      }
      x = seat.touch_x;
      y = seat.touch_y;
    }

    damage_whole();
  }

  void DragIcon::damage_whole()
  {
    for (auto& output : seat.input.server.desktop.outputs) {
      output.context.damage_whole_drag_icon(*this);
    }
  }

  void Seat::update_capabilities() noexcept
  {
    uint32_t caps = 0;
    if (!keyboards.empty()) {
      caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    if (!pointers.empty() || !tablets.empty()) {
      caps |= WL_SEAT_CAPABILITY_POINTER;
    }
    if (!touch.empty()) {
      caps |= WL_SEAT_CAPABILITY_TOUCH;
    }
    wlr_seat_set_capabilities(wlr_seat, caps);

    // Hide cursor if seat doesn't have pointer capability
    if ((caps & WL_SEAT_CAPABILITY_POINTER) == 0) {
      wlr_cursor_set_image(cursor.wlr_cursor, nullptr, 0, 0, 0, 0, 0, 0);
    } else {
      wlr_xcursor_manager_set_cursor_image(cursor.xcursor_manager, cursor.default_xcursor.c_str(),
                                           cursor.wlr_cursor);
    }
  }

  Device::Device(Seat& seat, wlr::input_device_t& device) noexcept : seat(seat), wlr_device(device)
  {
    device.data = this;
  }

  Pointer::Pointer(Seat& seat, wlr::input_device_t& device) noexcept : Device(seat, device)
  {
    assert(device.type == WLR_INPUT_DEVICE_POINTER);

    device.data = this;
    wlr_cursor_attach_input_device(seat.cursor.wlr_cursor, &device);

    on_device_destroy.add_to(device.events.destroy);
    on_device_destroy = [this] {
      auto& seat = this->seat;
      util::erase_this(this->seat.pointers, this);
      seat.update_capabilities();
    };
    seat.configure_cursor();
  }

  Touch::Touch(Seat& seat, wlr::input_device_t& device) noexcept : Device(seat, device)
  {
    assert(device.type == WLR_INPUT_DEVICE_TOUCH);

    device.data = this;
    wlr_cursor_attach_input_device(seat.cursor.wlr_cursor, &device);

    on_device_destroy.add_to(device.events.destroy);
    on_device_destroy = [this] {
      auto keep_around = util::erase_this(this->seat.touch, this);
      this->seat.update_capabilities();
    };

    seat.configure_cursor();
  }

  Pointer::~Pointer() noexcept
  {
    wlr_cursor_detach_input_device(seat.cursor.wlr_cursor, &wlr_device);
    this->seat.update_capabilities();
  }

  Touch::~Touch() noexcept
  {
    wlr_cursor_detach_input_device(seat.cursor.wlr_cursor, &wlr_device);
    this->seat.update_capabilities();
  }

  auto Seat::add_keyboard(wlr::input_device_t& device) -> Keyboard&
  {
    assert(device.type == WLR_INPUT_DEVICE_KEYBOARD);
    auto& kbd = keyboards.emplace_back(*this, device);
    wlr_seat_set_keyboard(wlr_seat, &device);
    return kbd;
  }

  auto Seat::add_pointer(wlr::input_device_t& device) -> Pointer&
  {
    assert(device.type == WLR_INPUT_DEVICE_POINTER);
    return pointers.emplace_back(*this, device);
  }

  auto Seat::add_touch(wlr::input_device_t& device) -> Touch&
  {
    assert(device.type == WLR_INPUT_DEVICE_TOUCH);
    return touch.emplace_back(*this, device);
  }

  auto Seat::add_tablet_pad(wlr::input_device_t& device) -> TabletPad&
  {
    assert(device.type == WLR_INPUT_DEVICE_TABLET_PAD);
    return tablet_pads.emplace_back(
      *this, *wlr_tablet_pad_create(input.server.desktop.tablet_v2, wlr_seat, &device));
  }

  auto Seat::add_tablet_tool(wlr::input_device_t& device) -> Tablet&
  {
    assert(device.type == WLR_INPUT_DEVICE_TABLET_TOOL);
    return tablets.emplace_back(*this, device);
  }

  auto Seat::add_device(wlr::input_device_t& device) noexcept -> Device&
  {
    auto& ref = [&]() -> Device& {
      switch (device.type) {
      case WLR_INPUT_DEVICE_KEYBOARD: return add_keyboard(device); break;
      case WLR_INPUT_DEVICE_POINTER: return add_pointer(device); break;
      case WLR_INPUT_DEVICE_TOUCH: return add_touch(device); break;
      case WLR_INPUT_DEVICE_TABLET_PAD: return add_tablet_pad(device); break;
      case WLR_INPUT_DEVICE_TABLET_TOOL: return add_tablet_tool(device); break;
      }
    }();

    configure_cursor();
    update_capabilities();
    return ref;
  }

  void Seat::configure_xcursor()
  {
    const char* cursor_theme = nullptr;
    Config::Cursor* cc = input.config.get_cursor(wlr_seat->name);

    if (cc != nullptr) {
      cursor_theme = cc->theme.c_str();
      if (!cc->default_image.empty()) {
        cursor.default_xcursor = cc->default_image;
      }
    }

    if (!cursor.xcursor_manager) {
      cursor.xcursor_manager = wlr_xcursor_manager_create(cursor_theme, xcursor_size);
      if (cursor.xcursor_manager == nullptr) {
        cloth_error("Cannot create XCursor manager for theme {}", cursor_theme);
        return;
      }
    }

    for (auto& output : input.server.desktop.outputs) {
      float scale = output.wlr_output.scale;
      if (wlr_xcursor_manager_load(cursor.xcursor_manager, scale)) {
        cloth_error("Cannot load xcursor theme for output '{}' with scale {}",
                    output.wlr_output.name, scale);
      }
    }

    wlr_xcursor_manager_set_cursor_image(cursor.xcursor_manager, cursor.default_xcursor.c_str(),
                                         cursor.wlr_cursor);
    wlr_cursor_warp(cursor.wlr_cursor, nullptr, cursor.wlr_cursor->x, cursor.wlr_cursor->y);
  }

  bool Seat::has_meta_pressed()
  {
    for (auto& keyboard : keyboards) {
      if (!keyboard.config.meta_key) {
        continue;
      }

      uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard.wlr_device.keyboard);
      if ((modifiers ^ keyboard.config.meta_key) == 0) {
        return true;
      }
    }

    return false;
  }

  View* Seat::get_focus()
  {
    return _focused_view;
  }

  SeatView::SeatView(Seat& p_seat, View& p_view) noexcept : seat(p_seat), view(p_view)
  {
    on_view_unmap = [this] { util::erase_this(seat.views, this); };
    on_view_unmap.add_to(view.events.unmap);
    on_view_destroy = [this] { util::erase_this(seat.views, this); };
    on_view_destroy.add_to(view.events.destroy);
  }

  SeatView::~SeatView() noexcept
  {
    if (&view == seat.get_focus()) {
      seat._focused_view = nullptr;
      seat.has_focus = false;
      seat.cursor.mode = Cursor::Mode::Passthrough;
    }

    if (this == seat.cursor.pointer_view) {
      seat.cursor.pointer_view = nullptr;
    }

    // Focus first view
    auto views = seat.input.server.desktop.visible_views();
    if (!views.empty()) {
      seat.set_focus(&views.back());
    }
  }

  void SeatView::deco_motion(double deco_sx, double deco_sy)
  {
    double sx = deco_sx;
    double sy = deco_sy;
    if (has_button_grab) {
      sx = grab_sx;
      sy = grab_sy;
    }

    DecoPart parts = view.deco.part_at(sx, sy);

    bool is_titlebar = parts & DecoPart::titlebar;
    wlr::edges_t edges = WLR_EDGE_NONE;
    if (parts & DecoPart::left_border) {
      edges |= WLR_EDGE_LEFT;
    } else if (parts & DecoPart::right_border) {
      edges |= WLR_EDGE_RIGHT;
    } else if (parts & DecoPart::bottom_border) {
      edges |= WLR_EDGE_BOTTOM;
    } else if (parts & DecoPart::top_border) {
      edges |= WLR_EDGE_TOP;
    }

    if (has_button_grab) {
      if (is_titlebar) {
        seat.begin_move(view);
      } else if (edges) {
        seat.begin_resize(view, edges);
      }
      has_button_grab = false;
    } else {
      if (is_titlebar) {
        wlr_xcursor_manager_set_cursor_image(
          seat.cursor.xcursor_manager, seat.cursor.default_xcursor.c_str(), seat.cursor.wlr_cursor);
      } else if (edges) {
        const char* resize_name = wlr_xcursor_get_resize_name(edges);
        wlr_xcursor_manager_set_cursor_image(seat.cursor.xcursor_manager, resize_name,
                                             seat.cursor.wlr_cursor);
      }
    }
  }

  void SeatView::deco_leave()
  {
    wlr_xcursor_manager_set_cursor_image(
      seat.cursor.xcursor_manager, seat.cursor.default_xcursor.c_str(), seat.cursor.wlr_cursor);
    has_button_grab = false;
  }

  void SeatView::deco_button(double sx, double sy, wlr::Button button, wlr::button_state_t state)
  {
    if (button == wlr::Button::left && state == WLR_BUTTON_PRESSED) {
      has_button_grab = true;
      grab_sx = sx;
      grab_sy = sy;
    } else {
      has_button_grab = false;
    }

    auto parts = view.deco.part_at(sx, sy);
    if (state == WLR_BUTTON_RELEASED && (parts & DecoPart::titlebar)) {
      wlr_xcursor_manager_set_cursor_image(
        seat.cursor.xcursor_manager, seat.cursor.default_xcursor.c_str(), seat.cursor.wlr_cursor);
    }
  }


  SeatView& Seat::add_view(View& view)
  {
    auto& seat_view = views.emplace_back(*this, view);
    return seat_view;
  }

  SeatView& Seat::seat_view_from_view(View& view)
  {
    auto found = util::find_if(views, [&](auto& sv) { return &sv.view == &view; });
    if (found == views.end()) {
      return add_view(view);
    }
    return *found;
  }

  Output* Seat::current_output()
  {
    auto* wlr_output = wlr_output_layout_output_at(input.server.desktop.layout,
                                                   cursor.wlr_cursor->x, cursor.wlr_cursor->y);
    if (!wlr_output) return nullptr;

    return input.server.desktop.output_from_wlr_output(wlr_output);
  }

  bool Seat::allow_input(wl::resource_t& resource)
  {
    return !exclusive_client || wl_resource_get_client(&resource) == exclusive_client;
  }

  void Seat::set_focus(View* view)
  {
    if (view && view->wlr_surface && !allow_input(*view->wlr_surface->resource)) {
      return;
    }

    SeatView* seat_view = nullptr;
    if (view != nullptr) {
      seat_view = &seat_view_from_view(*view);
      if (seat_view == nullptr) {
        return;
      }
    }

    has_focus = false;

    auto* prev_focus = get_focus();
    _focused_view = view;

    // Deactivate the old view if it is not focused by some other seat
    if (prev_focus != nullptr && !input.view_has_focus(*prev_focus)) {
      if (auto* xwl = dynamic_cast<XwaylandSurface*>(view);
          xwl && xwl->xwayland_surface->override_redirect) {
        // NOTE:
        // This may not be the correct thing to do, but popup menus in chrome instantly disappear if
        // the parent window gets deactivated
      } else {
        prev_focus->activate(false);
      }
    }

    if (view == nullptr) {
      cursor.mode = Cursor::Mode::Passthrough;
      wlr_seat_keyboard_clear_focus(wlr_seat);
      im_relay.set_focus(nullptr);
      return;
    }

    view->damage_whole();

    if (focused_layer) {
      return;
    }

    view->activate(true);
    has_focus = true;

    // An existing keyboard grab might try to deny setting focus, so cancel it
    wlr_seat_keyboard_end_grab(wlr_seat);

    wlr::keyboard_t* keyboard = wlr_seat_get_keyboard(wlr_seat);
    if (keyboard != nullptr) {
      wlr_seat_keyboard_notify_enter(wlr_seat, view->wlr_surface, keyboard->keycodes,
                                     keyboard->num_keycodes, &keyboard->modifiers);
      /* FIXME: Move this to a better place */
      for (auto& pad : tablet_pads) {
        if (pad.tablet) {
          wlr_send_tablet_v2_tablet_pad_enter(&pad.tablet_v2_pad, &pad.tablet->tablet_v2,
                                              view->wlr_surface);
        }
      }
    } else {
      wlr_seat_keyboard_notify_enter(wlr_seat, view->wlr_surface, nullptr, 0, nullptr);
    }


    cursor.update_focus();
    im_relay.set_focus(view->wlr_surface);
  }

  /**
   * Focus semantics of layer surfaces are somewhat detached from the normal focus
   * flow. For layers above the shell layer, for example, you cannot unfocus them.
   * You also cannot alt-tab between layer surfaces and shell surfaces.
   */
  void Seat::set_focus_layer(wlr::layer_surface_v1_t* layer)
  {
    if (!layer) {
      focused_layer = nullptr;
      set_focus(get_focus());
      return;
    }
    wlr::keyboard_t* keyboard = wlr_seat_get_keyboard(wlr_seat);
    if (!allow_input(*layer->resource)) {
      return;
    }
    if (has_focus) {
      View* prev_focus = get_focus();
      wlr_seat_keyboard_clear_focus(wlr_seat);
      prev_focus->activate(false);
    }
    has_focus = false;
    if (layer->layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP) {
      focused_layer = layer;
    }
    if (keyboard != nullptr) {
      wlr_seat_keyboard_notify_enter(wlr_seat, layer->surface, keyboard->keycodes,
                                     keyboard->num_keycodes, &keyboard->modifiers);
    } else {
      wlr_seat_keyboard_notify_enter(wlr_seat, layer->surface, nullptr, 0, nullptr);
    }
    cursor.update_focus();
  }

  void Seat::set_exclusive_client(wl::client_t* client)
  {
    if (!client) {
      exclusive_client = client;
      // Triggers a refocus of the topmost surface layer if necessary
      // TODO: Make layer surface focus per-output based on cursor position
      for (auto& output : input.server.desktop.outputs) {
        arrange_layers(output);
      }
      return;
    }
    if (focused_layer) {
      if (wl_resource_get_client(focused_layer->resource) != client) {
        set_focus_layer(nullptr);
      }
    }
    if (has_focus) {
      View* focus = get_focus();
      if (wl_resource_get_client(focus->wlr_surface->resource) != client) {
        set_focus(nullptr);
      }
    }
    if (wlr_seat->pointer_state.focused_client) {
      if (wlr_seat->pointer_state.focused_client->client != client) {
        wlr_seat_pointer_clear_focus(wlr_seat);
      }
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr::touch_point_t* point;
    wl_list_for_each(point, &wlr_seat->touch_state.touch_points, link)
    {
      if (point->client->client != client) {
        wlr_seat_touch_point_clear_focus(wlr_seat, now.tv_nsec / 1000, point->touch_id);
      }
    }
    exclusive_client = client;
  }

  void Seat::begin_move(View& view)
  {
    cursor.mode = Cursor::Mode::Move;
    cursor.offs_x = cursor.wlr_cursor->x;
    cursor.offs_y = cursor.wlr_cursor->y;
    if (view.maximized) {
      cursor.view_x = view.saved.x;
      cursor.view_y = view.saved.y;
    } else {
      cursor.view_x = view.x;
      cursor.view_y = view.y;
    }
    view.maximize(false);
    wlr_seat_pointer_clear_focus(wlr_seat);

    wlr_xcursor_manager_set_cursor_image(cursor.xcursor_manager, xcursor_move, cursor.wlr_cursor);
  }

  void Seat::begin_resize(View& view, wlr::edges_t edges)
  {
    cursor.mode = Cursor::Mode::Resize;
    cursor.offs_x = cursor.wlr_cursor->x;
    cursor.offs_y = cursor.wlr_cursor->y;
    if (view.maximized) {
      cursor.view_x = view.saved.x;
      cursor.view_y = view.saved.y;
      cursor.view_width = view.saved.width;
      cursor.view_height = view.saved.height;
    } else {
      cursor.view_x = view.x;
      cursor.view_y = view.y;
      wlr::box_t box = view.get_box();
      cursor.view_width = box.width;
      cursor.view_height = box.height;
    }
    cursor.resize_edges = edges;
    view.maximize(false);
    wlr_seat_pointer_clear_focus(wlr_seat);

    const char* resize_name = wlr_xcursor_get_resize_name(edges);
    wlr_xcursor_manager_set_cursor_image(cursor.xcursor_manager, resize_name, cursor.wlr_cursor);
  }

  void Seat::begin_rotate(View& view)
  {
    cursor.mode = Cursor::Mode::Rotate;
    cursor.offs_x = cursor.wlr_cursor->x;
    cursor.offs_y = cursor.wlr_cursor->y;
    cursor.view_rotation = view.rotation;
    view.maximize(false);
    wlr_seat_pointer_clear_focus(wlr_seat);

    wlr_xcursor_manager_set_cursor_image(cursor.xcursor_manager, xcursor_rotate, cursor.wlr_cursor);
  }

  void Seat::end_compositor_grab()
  {
    View* view = get_focus();
    if (view == nullptr) return;

    switch (cursor.mode) {
    case Cursor::Mode::Move: view->move(cursor.view_x, cursor.view_y); break;
    case Cursor::Mode::Resize:
      view->move_resize(cursor.view_x, cursor.view_y, cursor.view_width, cursor.view_height);
      break;
    case Cursor::Mode::Rotate: view->rotation = cursor.view_rotation; break;
    case Cursor::Mode::Passthrough: break;
    }

    cursor.mode = Cursor::Mode::Passthrough;
  }

  PointerConstraint::PointerConstraint(wlr::pointer_constraint_v1_t* wlr_constraint)
    : wlr_constraint(wlr_constraint)
  {
    on_destroy.add_to(wlr_constraint->events.destroy);
    on_destroy = [this](void* data) {
      auto* wlr_constraint = (wlr::pointer_constraint_v1_t*) data;
      auto& seat = *((Seat*) wlr_constraint->seat->data);
      if (seat.cursor.active_constraint == wlr_constraint) {
        seat.cursor.on_constraint_commit.remove();
        seat.cursor.active_constraint = nullptr;
        if (wlr_constraint->current.committed & WLR_POINTER_CONSTRAINT_V1_STATE_CURSOR_HINT &&
            seat.cursor.pointer_view) {
          double sx = wlr_constraint->current.cursor_hint.x;
          double sy = wlr_constraint->current.cursor_hint.y;
          View& view = seat.cursor.pointer_view->view;
          // TODO: rotate_child_position(&sx, &sy, 0, 0, view->width, view->height, view->rotation);
          double lx = view.x + sx;
          double ly = view.y + sy;
          wlr_cursor_warp(seat.cursor.wlr_cursor, nullptr, lx, ly);
        }
      }
      delete this;
    };

    auto& seat = *((Seat*) wlr_constraint->seat->data);
    double sx, sy;
    View* v;
    struct wlr_surface* surface = seat.input.server.desktop.surface_at(
      seat.cursor.wlr_cursor->x, seat.cursor.wlr_cursor->y, sx, sy, v);
    if (surface == wlr_constraint->surface) {
      assert(!seat.cursor.active_constraint);
      seat.cursor.constrain(wlr_constraint, sx, sy);
    }
  }

} // namespace cloth
