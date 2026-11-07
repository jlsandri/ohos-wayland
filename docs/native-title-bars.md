# Native title bars for GTK apps

A GTK 4 app on ohos-wayland shows a HarmonyOS title bar instead of its own header bar. ArkUI draws the bar with HarmonyOS buttons, menus, symbols, and colours. The real HarmonyOS maximize, minimize, and close buttons sit on it. The app keeps its behaviour, because every click runs the same GTK action or widget the header bar would have run.

This page explains how that works and why it is built this way. For the manifest fields, see [the bottle reference](bottle-reference.md#title-bar-overrides).

## Why not style the GTK header bar

A styled header bar still behaves like GTK. Its menus are GTK popovers, its icons are Adwaita, and its height and spacing follow libadwaita. The first attempt (commit 8b43c9b) went that way. `app/entry/src/main/ets/common/TitleBarStyle.ets` generates density-scaled CSS that copies the ArkUI window decor: a 37vp bar and 28vp buttons, with the `ContainerModalTheme` colours. Those constants are in `container_modal_constants.h` of `arkui_ace_engine`. The result looked close in a screenshot and wrong in use. Menus did not open like HarmonyOS menus. The window buttons were GTK drawings of HarmonyOS buttons, not the system buttons.

Native HarmonyOS apps with custom title bars hide the system title but keep the system window buttons. Huawei's "How do I customize the title bar in a free window?" FAQ describes exactly that pattern (`setWindowDecorVisible`, `setWindowTitleButtonVisible`, `setWindowDecorHeight(56)`). To get that result for a GTK app, something has to read the header bar's contents and hand them to ArkUI. The GTK header bar is public widget API, so a library inside the app process can read it without patching GTK.

The styled header bar stays as the fallback. A header bar the bridge cannot describe keeps its GTK drawing with the HarmonyOS CSS.

## The parts

| Part | File | Runs in |
| --- | --- | --- |
| Protocol | `app/protocols/ohos-titlebar-v1.xml` | Wayland wire |
| Bridge | `app/entry/src/main/cpp/runtime/gtk_titlebar_bridge.cpp`, built as `libohos_titlebar.so` | The app process, on the GTK main thread |
| Compositor global | `app/entry/src/main/cpp/compositor/titlebar.{h,cpp}` | The Wayland thread of the HAP process |
| ArkUI title bar | `app/entry/src/main/ets/common/NativeTitleBar.ets` | The HAP UI thread |
| Symbol map | `app/entry/src/main/ets/common/SymbolMap.ets` | The HAP UI thread |
| Wiring | `app/entry/src/main/ets/pages/Index.ets` | The HAP UI thread |

`ohos_wl.py bottle` adds `libohos_titlebar.so` to any bottle whose library closure contains `libgtk-4.so.1` (`TITLEBAR_BRIDGE`). The direct child loads it in `LoadTitlebarBridge` (`app/entry/src/main/cpp/runtime/direct_child.cpp`). It checks for GTK with `dlopen("libgtk-4.so.1", RTLD_NOLOAD)`, and then it `dlopen`s the bridge from the app's `libs` directory. The bridge's constructor adds a 250 ms GLib timeout. That timer is the only way the bridge runs, so the app needs no change and no rebuild.

## The protocol

`ohos_titlebar_manager_v1` is a global with one request, `get_titlebar(new_id, wl_surface)`. The surface is a toplevel's `wl_surface`. The returned `ohos_titlebar_v1` has three requests and one event.

| Message | Direction | Meaning |
| --- | --- | --- |
| `append(string chunk)` | client to compositor | Adds text to the pending description. |
| `commit` | client to compositor | Makes the pending text the current description and starts a new one. |
| `destroy` | client to compositor | Hands the title bar back to the client. The compositor posts `{"native":false}`. |
| `activate(string)` | compositor to client | The user activated an item. |

The description goes in chunks because one Wayland message is limited in size. The bridge sends 2048-byte chunks (`kChunk`). A menu-heavy header bar easily passes that limit.

The `activate` wire format is a tab-separated string, `serial\tid\taction\ttarget`. `Index.ets` builds it, `ActivateTitlebarItem` in `bridge/napi_init.cpp` passes it through unchanged, and `OnActivate` in the bridge splits it on tabs.

The compositor resolves the surface to a toplevel id with `FindToplevelBySurface` and posts each committed description as a `titlebar` toplevel event to ArkTS. A description for a surface that is not yet a mapped toplevel is dropped with a log line. The bridge sends the description again on its next poll.

Activations start on the UI thread and must reach a `wl_resource` that only the Wayland thread may touch. `ActivateTitlebar` puts the message on a mutex-guarded queue and writes one byte to a pipe. The Wayland event loop watches the pipe (`OnActivationsReady`), drains the queue, and sends `activate` to the toplevel's title bar.

## The description

The description is JSON. Its shape is the `TitleBarModel`, `TitleBarItem`, `MenuEntry`, and `TitleBarTab` interfaces in `NativeTitleBar.ets`.

```json
{
	"serial": 7,
	"native": true,
	"title": "~/Projects",
	"subtitle": "optional",
	"start": [ items ],
	"center": [ items ],
	"end": [ items ],
	"tabs": [ { "id": 9, "title": "zsh", "selected": true, "loading": false, "attention": false } ]
}
```

Each item has a `kind` and an `id`. It may also carry `icon`, `iconFile`, `label`, `tooltip`, `disabled`, `active`, `style` (`suggested` or `destructive`), `items` (a menu), `options` (switcher options), and `dropdownTooltip`. Menu entries have `kind` (`item`, `section`, or `submenu`), `label`, `action`, `target`, `accel`, `checked`, `disabled`, and `items`.

The id is an index into the bridge's `items` vector. The bridge rebuilds that vector with each description and holds a reference to each widget or `AdwTabPage` in it. The serial goes up only when the description text changes. `OnActivate` ignores an activation whose serial is not the current one, so a click on a stale bar cannot hit the wrong widget.

`{"native":false}` means the toolkit draws its own bar. ArkTS then removes the native bar and restores the HarmonyOS decor state.

## How the bridge reads a header bar

Each poll, the bridge walks `gtk_window_get_toplevels()` and skips windows that have a transient parent, because dialogs keep their own header bars. For each window it finds the first visible `GtkHeaderBar` or `AdwHeaderBar` (`FindShown`). It skips a header bar the app hid itself. Ghostty with `gtk-titlebar-style = tabs` hides one, and that hidden bar is not the title bar. The bridge then finds the header bar's `GtkCenterBox` and describes its start, center, and end children.

The translation rules are in `Describer::Item`:

| GTK widget | Description |
| --- | --- |
| hidden widget, `GtkWindowControls` | Nothing. The system buttons replace window controls. |
| `AdwWindowTitle` | Sets `title` and `subtitle`. |
| `GtkLabel` with the `title` CSS class | Sets `title`. |
| other `GtkLabel` | `label` |
| `AdwSplitButton` | `split`, with its `menu-model` and `dropdown-tooltip`. |
| `GtkMenuButton` with a `GMenuModel` | `menu`. With no icon and no label, it gets `open-menu-symbolic`. |
| `GtkMenuButton` with a custom popover and no model | Not translatable. |
| `GtkStackSwitcher` | `switcher`, one option per visible toggle button, with `active`. |
| `GtkButton`, `GtkToggleButton` | `button`, with its icon name or label from the button or its child (`GtkImage`, `GtkLabel`, `AdwButtonContent`). A toggle carries `active`. |
| `GtkSeparator` | `separator` |
| `GtkImage` with an icon name | `icon` |
| `GtkSpinner`, `AdwSpinner` | Nothing. |
| a container with children | Its children, flattened. |
| any other leaf widget | Not translatable. |

Every item records its tooltip, sensitivity, and the `suggested-action` or `destructive-action` CSS class. When an icon name resolves to a file in the GTK icon theme, the item also carries `iconFile`. The bridge looks each name up once and caches the path (commit 770dc11).

Menus come from the `GMenuModel`. Sections and submenus keep their labels. Each item records its action and target. The bridge asks the action's owner (`app.` on the `GApplication`, `win.` on the window) whether the action is enabled and what its state is. A boolean state becomes `checked`, and a state equal to the item's target marks a radio item as checked. The first accelerator from `gtk_application_get_accels_for_action` becomes `accel`, formatted by `gtk_accelerator_get_label`. A menu item that names a `custom` widget slot is not translatable.

An `AdwTabBar` adds `tabs`. The bridge reads the tab bar's `view` property, then the view's `pages` list model and `selected-page`, and each page's `title`, `loading`, and `needs-attention`. All of these are GObject properties, so the bridge does not link libadwaita.

### Fallback

One untranslatable widget anywhere in the header bar makes the whole description `{"native":false}`. The bridge then shows the header bar and the tab bar again (`Restore`), and GTK draws them with the HarmonyOS CSS. A half-native bar would lose the one control the bridge cannot draw, so the bridge does not attempt one.

When the description is native, the bridge hides the header bar and the tab bar with `gtk_widget_set_visible(FALSE)`. It keeps pointers to the hidden widgets, because `FindShown` would no longer find them.

### Activation

| Message | What the bridge runs |
| --- | --- |
| button, switcher option, or split button main part, with an empty action | `gtk_widget_activate` on that widget. |
| menu item | `gtk_widget_activate_action_variant(menuButton, action, target)`, so the action resolves through the widget's own action muxer, as the GTK popover would resolve it. The target is parsed with `g_variant_parse`. |
| tab, action `close` | `adw_tab_view_close_page`, found with `dlsym(RTLD_DEFAULT)`. The app can still refuse or confirm the close. |
| tab, any other action | Sets the view's `selected-page`. |

### Cost

The poll runs on the GTK main thread every 250 ms and builds the description string each time. A slow poll would show as a hitch in the app's animations. The bridge logs `ohos-titlebar: describing the header bar took N us` when a poll takes over 4 ms (commit 770dc11). The typical poll cost has not been measured. Change notification through `notify` signals would remove the poll. It is not built, because the bridge would then need a signal for every property it reads.

## Layout by role

GTK packs header bar items in the order the app chose, at start or end. HarmonyOS title bars order items by what they do. The native bar ignores GTK packing and puts each item in a slot (`slotOf` in `NativeTitleBar.ets`):

| Slot | Items | Position |
| --- | --- | --- |
| `lead` | `go-previous`, `go-next`, `go-home`, `go-up`, `sidebar-show`, `sidebar-hide`, `sidebar-show-right`, `view-sidebar-start`, `view-sidebar-end` | Before the title. |
| `afterTitle` | `tab-new` | Right after the title, or after the tabs. The HarmonyOS browser places new tab there. |
| `center` | every `switcher` | Centred on the bar, over the other slots. |
| `actions` | everything else | Right side. |
| `menu` | a `menu` whose icon is `open-menu`, `view-more`, or `view-more-horizontal`, or a `menu` with no icon | Last, next to the window buttons. |

A bottle's `titlebar` object overrides the slot for an icon name. With more than one tab, the tab strip replaces the title. Each tab is 200vp wide with its own close button, and the strip scrolls sideways.

The bar reserves space on the right for the system buttons. `getTitleButtonRect()` reports their width, and the bar pads by that width plus 8vp. The default is 148vp, which is the 140vp the call has reported plus that gap. The `TITLE_BUTTON_RECT` log line records the value on each launch.

A drag on empty bar space calls `window.startMoving()` after the first `TouchType.Move` past 4vp. A double tap toggles maximize. Items stop touch propagation, so clicking a button does not start a drag.

## Sizes and the evidence for them

| Size | Value | Evidence |
| --- | --- | --- |
| Bar height | 56vp | Huawei's free-window FAQ calls `setWindowDecorHeight(56)`. HiShell's title area measured 106 px in a window at y=0, and 106 / 1.9 = 55.8vp. |
| Button size | 40vp | ArkUI's title button response region is 40vp (`TITLE_BUTTON_RESPONSE_REGIOIN_WIDTH` in `container_modal_constants.h`). HiShell's buttons measure 76 by 74 px, about 40 by 39vp. |
| Glyph size | 20vp | HiShell's maximize, minimize, and close images are 38 by 38 px, and 38 / 1.9 = 20vp. |
| Window corner radius | 16vp | `CONTAINER_OUTER_RADIUS = 16.0_vp` in `container_modal_constants.h`. |
| System button reserve | 140vp plus 8vp | `getTitleButtonRect()` on the device, logged as `TITLE_BUTTON_RECT`. The 140vp value comes from the session that wrote commit 9b9bed2. No log line is saved in the repo. |

The measurements in this table used a display density of 1.9 (`hidumper -s DisplayManagerService -a -a`).

The page clips to the 16vp radius over a transparent window background (`Index.ets`, `WINDOW_RADIUS`), and it is square when the window is maximized. GTK draws square corners, so the app's background does not show past the rounded edge.

## Fullscreen and maximized title bars

`FULL_SCREEN` uses the HarmonyOS hover path. `Index.ets` calls `setTitleAndDockHoverShown(true, true)`. Hovering at the top shows the native title and status bar. Hovering at the bottom shows the dock. Moving away hides them. The system window buttons remain visible. No ArkUI title overlay is mounted in this state, so there is only one title bar.

Apps reach `MAXIMIZE` through the maximize button, an xdg_toplevel request, and reopening, because `win.maximize()` with no argument means fullscreen. An app with `topBar` in its bottle is the exception: it goes fullscreen, because its own bar has the window buttons.

`MAXIMIZE` keeps the native title bar at the top of the window, in the layout, as a floating window does, and adds the window buttons. An app without a translated model gets a shell model with the window title and window controls. The app sits below the bar with no top inset. The status bar still shows over it on hover.

A bottle can set `titlebarMode` to `hover` instead. The app then fills the window, and an ArkUI overlay shows the bar. The overlay appears when the pointer leaves the window while rising through its top half, because the status bar takes the pointer as it shows. The overlay hides when the pointer moves into the app. Its frosted backdrop runs behind the status bar, because HarmonyOS hides the status bar when the pointer moves off it. The overlay slides in over 250 ms and out over 600 ms, which is an estimate from the status bar's layer state.

The two bars do not animate in step, so `fixed` is the default. The overlay infers the status bar from the pointer, because no window API reports the status bar. These events stayed silent across three hover rounds:

- `avoidAreaChange`
- `systemAvoidAreaChange`
- `occlusionStateChanged`
- `windowEvent`
- `windowHighlightChange`
- `windowVisibilityChange`
- `windowRectChange`

Polling the avoid areas and the system bar properties every 30 ms showed no change either.

HarmonyOS keeps its floating title even after `setWindowDecorVisible(false)`. Disabling that title with `setTitleAndDockHoverShown(false, ...)` also removes status and dock hover. Direct system-bar calls do not replace the native path on this PC. This is why fullscreen uses the system title and maximized windows use the native title bar.

## Margins around the app

The manifest's `background` is the startup color, shown until the first frame. A window that fills the screen keeps the app inside the insets from `DisplayCorners.ets`, so its corners clear the panel's. `Index.ets` passes them to the renderer with `setContentInsets`, and apps lay out at the size that leaves. The XComponent still covers the window: both render paths draw over the whole surface and clamp at the app's edge, so its outermost pixels continue across the insets. Nothing samples or guesses a color, so each edge matches whatever the app draws there, on every frame. Anything touching the app's outermost row stretches by up to the inset, such as a block cursor on the top row of a terminal with no padding. Pixels past the rounded corners are black, like the bezel. A pointer over the insets acts on the app's nearest edge.

A bar along the top keeps no top inset, whether it is the native title bar or an app's own bar with `topBar` set. A stretched row there would make the bar taller and push its contents off-centre, and the rounded corners only crop the bar's background.

## Icons

An item's `icon` is a freedesktop icon name. `SymbolMap.ets` maps names to HarmonyOS system symbols (`sys.symbol.*`). Those are the HM Symbol glyphs that HarmonyOS's own title bars use, and ArkUI draws them with `SymbolGlyph` in the system icon colour. If no symbol is mapped, the bar draws `iconFile`, the SVG the GTK theme resolved, tinted with the same colour. If neither exists, the button has no glyph.

To add a mapping:

1. Find the icon name the app uses. It is the `icon-name` of the header bar widget in the app's source or `.ui` file. No log line prints the description JSON today.
2. Pick the HarmonyOS symbol that HarmonyOS uses for the same job. Where ArkUI has a precedent, use it. Navigation uses `chevron_backward` for back and `dot_grid_2x2` for more (`navigation_bar_theme.cpp:88-89`).
3. Confirm that the symbol exists in the SDK. The symbol names are keys in `ets/build-tools/ets-loader/sysResource.js` under the SDK's `openharmony` directory.
4. Add the entry to `SYMBOLS` in `SymbolMap.ets`, without the `-symbolic` suffix.

An alias is for a button whose GTK icon says less than the button does. Ghostty's tab overview button uses `view-grid-symbolic`, which maps to a plain grid. `apps/ghostty/bottle.json` aliases `view-grid` to `tab-overview`, and `SymbolMap.ets` draws `tab-overview` as `rectangle_stack`. Put an alias in the bottle when the meaning belongs to one app, and in `SymbolMap.ets` when it is true for every app.

## Close

The close button is the HarmonyOS one. The launcher registers `windowWillClose`. On the first click it calls `closeToplevel()`, which sends `xdg_toplevel.close`, and it holds the window open. GTK then does what the app does on close. It may ask to save, confirm, or close one tab. When the app destroys its toplevel, the launcher stops the program and calls `terminateSelf()`. A second click within 3 seconds closes the window even if the app has not answered, so a hung app can still be closed.

Maximize and minimize are the system buttons too. `windowStatusChange` tells the app through `setToplevelMaximized`, so GTK draws its maximized state.

## Verification so far

Commit b23ffd0 records the checks on the GTK demo (`native_gtk_demo.cpp`, test `gtk-demo`). The switcher switches the GTK stack. A radio menu item runs `win.density('compact')` and comes back checked. The close button closes the app through GTK. `ohos_wl.py screenshot testing --window` captures the whole window with its title bar through `window.snapshot`. `build/screens/native-titlebar.png` is such a capture. It shows the title, the centred switcher, the new-item and menu buttons, and the system buttons. `build/` is ignored by Git.

Commit d0c42eb describes Ghostty's translated header bar: new tab with its split menu, tab overview, main menu, and the title with the working directory. No capture of it is saved.

On 2026-11-07, a window capture of `gtk-demo` in `MAXIMIZE` showed the fixed bar at the top with the app below it. `fallback-bar` and `gtk-bar` check that the bar is on, and `fallback-hover` and `gtk-hover` replay the overlay's reveal and hide. These four tests run in hidden windows and cannot show the status bar, so a real pointer check on the testing app covered the hover overlay.

## Extending to other toolkits

The protocol knows nothing about GTK. A toolkit needs four things to use it:

1. A way into the app process. GTK apps get the bridge through `LoadTitlebarBridge`. A toolkit the app links directly, such as gpui in Zed, would bind `ohos_titlebar_manager_v1` itself, and that needs a toolkit patch.
2. A model of its title bar's contents: buttons with icons and labels, menus with actions and state, switchers, and tabs. Qt has `QAction` and `QMenu`, which map onto `MenuEntry` the same way `GMenuModel` does. Toolkits that draw arbitrary widgets in the title bar need a fallback rule like GTK's.
3. Stable ids per description, and a check of the serial on activation.
4. A way to hide its own title bar while the native one shows. On `{"native":false}`, or when the title bar object is destroyed, it shows its own bar again.

The icon names should be freedesktop names, so `SymbolMap.ets` and the bottle aliases work unchanged. Before a second toolkit uses the protocol, give the protocol a versioned description schema. Today the schema lives only in the TypeScript interfaces.
