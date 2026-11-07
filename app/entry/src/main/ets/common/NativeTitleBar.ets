import { symbolFor } from './SymbolMap';

// A toolkit header bar drawn as a HarmonyOS title bar. The description comes
// from the app (gtk_titlebar_bridge.cpp through the "titlebar" toplevel
// event); items are activated by id, menu items by action. The HarmonyOS
// maximize, minimize, and close buttons float over the reserved right edge.

export interface MenuEntry {
  kind: string;               // item, section, or submenu
  label?: string;
  action?: string;
  target?: string;
  accel?: string;
  checked?: boolean;
  disabled?: boolean;
  items?: MenuEntry[];
}

export interface TitleBarItem {
  kind: string;               // button, menu, switcher, label, icon, separator
  id: number;
  icon?: string;
  iconFile?: string;
  label?: string;
  tooltip?: string;
  disabled?: boolean;
  active?: boolean;
  style?: string;             // suggested or destructive
  items?: MenuEntry[];
  options?: TitleBarItem[];
  dropdownTooltip?: string;
}

export interface TitleBarTab {
  id: number;
  title: string;
  selected?: boolean;
  loading?: boolean;
  attention?: boolean;
}

export interface TitleBarModel {
  serial: number;
  native: boolean;
  title?: string;
  subtitle?: string;
  start?: TitleBarItem[];
  center?: TitleBarItem[];
  end?: TitleBarItem[];
  tabs?: TitleBarTab[];
}

// Sizes of the HarmonyOS custom title bar (56vp, as HiShell and the
// HarmonyOS free-window guide use): 40vp buttons whose 20vp glyphs match
// the system maximize, minimize, and close glyphs beside them.
const BUTTON_SIZE = 40;
const ICON_SIZE = 20;
const TAB_WIDTH = 200;

export type TitleBarActivate = (id: number, action: string, target: string) => void;

// Menu sections show as groups; a submenu shows as a group with its label.
function menuGroups(items: MenuEntry[]): MenuEntry[] {
  const groups: MenuEntry[] = [];
  let loose: MenuEntry[] = [];
  for (const entry of items) {
    if (entry.kind === 'item') {
      loose.push(entry);
      continue;
    }
    if (loose.length > 0) groups.push({ kind: 'section', items: loose });
    loose = [];
    groups.push({ kind: 'section', label: entry.label,
      items: (entry.items ?? []).filter((child: MenuEntry) => child.kind === 'item') });
    for (const child of entry.items ?? []) {
      if (child.kind !== 'item') groups.push(...menuGroups([child]));
    }
  }
  if (loose.length > 0) groups.push({ kind: 'section', items: loose });
  return groups.filter((group: MenuEntry) => (group.items ?? []).length > 0);
}

// An item's icon name without its -symbolic suffix.
export function iconName(item: TitleBarItem): string {
  return (item.icon ?? '').replace(/-symbolic(-rtl|-ltr)?$/, '');
}

// Where a HarmonyOS title bar puts an item, by what it does: navigation
// before the title, new tab right after the title or tabs (as the HarmonyOS
// browser does), view switchers in the middle, actions at the right, and the
// app menu last, beside the window buttons.
const NAVIGATION = ['go-previous', 'go-next', 'go-home', 'go-up', 'sidebar-show', 'sidebar-hide',
  'sidebar-show-right', 'view-sidebar-start', 'view-sidebar-end'];
const AFTER_TITLE = ['tab-new'];
const APP_MENU = ['open-menu', 'view-more', 'view-more-horizontal'];

export function slotOf(item: TitleBarItem): string {
  const icon = iconName(item);
  if (item.kind === 'switcher') return 'center';
  if (NAVIGATION.includes(icon)) return 'lead';
  if (AFTER_TITLE.includes(icon)) return 'afterTitle';
  if (item.kind === 'menu' && (APP_MENU.includes(icon) || icon === '')) return 'menu';
  return 'actions';
}

// GTK menu labels mark mnemonics with an underscore.
function plainLabel(label?: string): string {
  return (label ?? '').replace(/_(.)/g, '$1');
}

@Component
struct TitleBarIcon {
  @Prop item: TitleBarItem;

  build() {
    if (symbolFor(this.item.icon)) {
      SymbolGlyph(symbolFor(this.item.icon))
        .fontSize(ICON_SIZE)
        .fontColor([this.item.style === 'destructive' ? $r('sys.color.warning') : $r('sys.color.icon_primary')])
    } else if (this.item.iconFile) {
      Image(`file://${this.item.iconFile}`)
        .width(ICON_SIZE)
        .height(ICON_SIZE)
        .fillColor(this.item.style === 'destructive' ? $r('sys.color.warning') : $r('sys.color.icon_primary'))
        .draggable(false)
    }
  }
}

@Component
struct TitleBarMenuButton {
  @Prop item: TitleBarItem;
  // A split button's dropdown: a small chevron instead of the item's icon.
  @Prop chevron: boolean = false;
  activate: TitleBarActivate = () => {};

  @Builder
  menu() {
    Menu() {
      ForEach(menuGroups(this.item.items ?? []), (group: MenuEntry) => {
        MenuItemGroup({ header: group.label ? plainLabel(group.label) : undefined }) {
          ForEach(group.items ?? [], (entry: MenuEntry) => {
            MenuItem({ content: plainLabel(entry.label), labelInfo: entry.accel })
              .selectIcon(entry.checked !== undefined)
              .selected(entry.checked === true)
              .enabled(entry.disabled !== true)
              .onClick(() => this.activate(this.item.id, entry.action ?? '', entry.target ?? ''))
          })
        }
      })
    }
  }

  build() {
    if (this.chevron) {
      Button({ type: ButtonType.Circle }) {
        SymbolGlyph($r('sys.symbol.chevron_down'))
          .fontSize(14)
          .fontColor([$r('sys.color.icon_secondary')])
      }
      .width(24)
      .height(BUTTON_SIZE)
      .backgroundColor(Color.Transparent)
      .accessibilityText(this.item.dropdownTooltip ?? '')
      .bindMenu(this.menu)
    } else if (this.item.label && !this.item.icon) {
      Button(plainLabel(this.item.label), { type: ButtonType.Capsule })
        .height(BUTTON_SIZE)
        .fontSize(16)
        .fontColor($r('sys.color.font_primary'))
        .backgroundColor($r('sys.color.comp_background_tertiary'))
        .enabled(this.item.disabled !== true)
        .bindMenu(this.menu)
    } else {
      Button({ type: ButtonType.Circle }) {
        TitleBarIcon({ item: this.item })
      }
      .width(BUTTON_SIZE)
      .height(BUTTON_SIZE)
      .backgroundColor(Color.Transparent)
      .enabled(this.item.disabled !== true)
      .accessibilityText(this.item.tooltip ?? this.item.label ?? '')
      .bindMenu(this.menu)
    }
  }
}

@Component
struct TitleBarItemView {
  @Prop item: TitleBarItem;
  activate: TitleBarActivate = () => {};

  build() {
    if (this.item.kind === 'menu') {
      TitleBarMenuButton({ item: this.item, activate: this.activate })
    } else if (this.item.kind === 'split') {
      Row() {
        Button({ type: ButtonType.Circle }) {
          TitleBarIcon({ item: this.item })
        }
        .width(BUTTON_SIZE)
        .height(BUTTON_SIZE)
        .backgroundColor(Color.Transparent)
        .enabled(this.item.disabled !== true)
        .accessibilityText(this.item.tooltip ?? '')
        .onClick(() => this.activate(this.item.id, '', ''))
        if ((this.item.items ?? []).length > 0) {
          TitleBarMenuButton({ item: this.item, chevron: true, activate: this.activate })
        }
      }
    } else if (this.item.kind === 'switcher') {
      Row() {
        ForEach(this.item.options ?? [], (option: TitleBarItem) => {
          Row() {
            if (option.icon) {
              TitleBarIcon({ item: option })
            } else {
              Text(plainLabel(option.label))
                .fontSize(16)
                .fontWeight(option.active ? FontWeight.Medium : FontWeight.Regular)
                .fontColor(option.active ? $r('sys.color.font_primary') : $r('sys.color.font_secondary'))
            }
          }
          .height(BUTTON_SIZE - 4)
          .padding({ left: 16, right: 16 })
          .borderRadius((BUTTON_SIZE - 4) / 2)
          .backgroundColor(option.active ? $r('sys.color.comp_background_primary') : Color.Transparent)
          .shadow(option.active ? ShadowStyle.OUTER_DEFAULT_XS : undefined)
          .onClick(() => this.activate(option.id, '', ''))
        })
      }
      .height(BUTTON_SIZE)
      .padding(2)
      .borderRadius(BUTTON_SIZE / 2)
      .backgroundColor($r('sys.color.comp_background_tertiary'))
    } else if (this.item.kind === 'button' && this.item.icon) {
      Button({ type: ButtonType.Circle }) {
        TitleBarIcon({ item: this.item })
      }
      .width(BUTTON_SIZE)
      .height(BUTTON_SIZE)
      .backgroundColor(this.item.active ? $r('sys.color.comp_background_tertiary') : Color.Transparent)
      .enabled(this.item.disabled !== true)
      .accessibilityText(this.item.tooltip ?? this.item.label ?? '')
      .onClick(() => this.activate(this.item.id, '', ''))
    } else if (this.item.kind === 'button') {
      Button(plainLabel(this.item.label), { type: ButtonType.Capsule })
        .height(BUTTON_SIZE)
        .fontSize(16)
        .fontColor(this.item.style ? Color.White : $r('sys.color.font_primary'))
        .backgroundColor(this.item.style === 'suggested' ? $r('sys.color.brand') :
          this.item.style === 'destructive' ? $r('sys.color.warning') : $r('sys.color.comp_background_tertiary'))
        .enabled(this.item.disabled !== true)
        .onClick(() => this.activate(this.item.id, '', ''))
    } else if (this.item.kind === 'label') {
      Text(this.item.label).fontSize(16).fontColor($r('sys.color.font_secondary'))
    } else if (this.item.kind === 'icon') {
      TitleBarIcon({ item: this.item })
    } else if (this.item.kind === 'separator') {
      Divider().vertical(true).height(24).color($r('sys.color.comp_divider'))
    }
  }
}

// How far a press must travel before it becomes a window move, in vp.
const DRAG_THRESHOLD_VP = 4;

@Component
export struct NativeTitleBar {
  @Prop model: TitleBarModel;
  @Prop barHeight: number = 56;
  // Room for the HarmonyOS window buttons at the right edge, in vp.
  @Prop systemButtons: number = 140;
  // The bar draws the window buttons itself: HarmonyOS's are hidden while it overlays a
  // window that fills the screen.
  @Prop ownWindowButtons: boolean = false;
  @Prop frosted: boolean = false;
  private pressX: number = 0;
  private pressY: number = 0;
  private moving: boolean = false;
  activate: TitleBarActivate = () => {};
  startMoving: () => void = () => {};
  toggleMaximized: () => void = () => {};
  minimize: () => void = () => {};
  close: () => void = () => {};

  // Per-app placements by icon name (bottle manifest "titlebar"), over the
  // role rules in slotOf.
  @Prop placements: Record<string, string> = {};

  private slotItems(slot: string): TitleBarItem[] {
    const all = (this.model.start ?? []).concat(this.model.center ?? [], this.model.end ?? []);
    return all.filter((item: TitleBarItem) => (this.placements[iconName(item)] ?? slotOf(item)) === slot);
  }

  // Builders read this.model directly: builder arguments are copies and
  // would not redraw when a new description arrives. So the slot name is
  // passed, and ForEach reads the items itself.
  @Builder
  slot(slot: string) {
    ForEach(this.slotItems(slot), (item: TitleBarItem) => {
      TitleBarItemView({ item: item, activate: this.activate })
        .onTouch((event: TouchEvent) => event.stopPropagation())
    }, (item: TitleBarItem) => JSON.stringify(item))
  }

  @Builder
  windowButton(symbol: string, action: () => void) {
    Button({ type: ButtonType.Normal }) {
      SymbolGlyph($r(symbol))
        .fontSize(20)
        .fontColor([$r('sys.color.icon_primary')])
    }
    .width(40)
    .height(40)
    .borderRadius(8)
    .backgroundColor(Color.Transparent)
    .onClick(action)
  }

  @Builder
  leadItems() {
    this.slot('lead')
  }

  @Builder
  afterTitleItems() {
    this.slot('afterTitle')
  }

  @Builder
  centerItems() {
    this.slot('center')
  }

  @Builder
  actionItems() {
    this.slot('actions')
  }

  @Builder
  menuItems() {
    this.slot('menu')
  }

  // Several tabs replace the title: HarmonyOS tabs in the title bar, each
  // with its close button.
  @Builder
  tabStrip() {
    Scroll() {
      Row({ space: 4 }) {
        ForEach(this.model.tabs ?? [], (tab: TitleBarTab) => {
          Row({ space: 4 }) {
            if (tab.attention) {
              Circle().width(6).height(6).fill($r('sys.color.brand'))
            }
            Text(tab.title)
              .fontSize(14)
              .fontWeight(tab.selected ? FontWeight.Medium : FontWeight.Regular)
              .fontColor(tab.selected ? $r('sys.color.font_primary') : $r('sys.color.font_secondary'))
              .maxLines(1)
              .textOverflow({ overflow: TextOverflow.Ellipsis })
              .layoutWeight(1)
            Button({ type: ButtonType.Circle }) {
              SymbolGlyph($r('sys.symbol.xmark'))
                .fontSize(12)
                .fontColor([$r('sys.color.icon_secondary')])
            }
            .width(20)
            .height(20)
            .backgroundColor(Color.Transparent)
            .onClick(() => this.activate(tab.id, 'close', ''))
          }
          .width(TAB_WIDTH)
          .height(BUTTON_SIZE)
          .padding({ left: 12, right: 8 })
          .borderRadius(12)
          .backgroundColor(tab.selected ? $r('sys.color.comp_background_tertiary') : Color.Transparent)
          .onClick(() => this.activate(tab.id, '', ''))
          .onTouch((event: TouchEvent) => event.stopPropagation())
        }, (tab: TitleBarTab) => JSON.stringify(tab))
      }
    }
    .scrollable(ScrollDirection.Horizontal)
    .scrollBar(BarState.Off)
    .flexShrink(1)
    .align(Alignment.Start)
  }

  build() {
    Stack() {
      Row({ space: 8 }) {
        this.leadItems()
        if ((this.model.tabs ?? []).length > 1) {
          this.tabStrip()
        } else {
        Column() {
          Text(this.model.title ?? '')
            .fontSize(16)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('sys.color.font_primary'))
            .maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
          if (this.model.subtitle) {
            Text(this.model.subtitle)
              .fontSize(12)
              .fontColor($r('sys.color.font_secondary'))
              .maxLines(1)
              .textOverflow({ overflow: TextOverflow.Ellipsis })
          }
        }
        .alignItems(HorizontalAlign.Start)
        .flexShrink(1)
        .margin({ left: 4, right: 4 })
        }
        this.afterTitleItems()
        Blank()
        this.actionItems()
        this.menuItems()
      }
      .width('100%')
      .height('100%')
      .padding({ left: 24, right: this.systemButtons })

      Row({ space: 8 }) {
        this.centerItems()
      }
      .hitTestBehavior(HitTestMode.Transparent)

      if (this.ownWindowButtons) {
        Row({ space: 4 }) {
          this.windowButton('sys.symbol.arrow_down_right_and_arrow_up_left', () => this.toggleMaximized())
          this.windowButton('sys.symbol.minus', () => this.minimize())
          this.windowButton('sys.symbol.xmark', () => this.close())
        }
        .width(this.systemButtons)
        .height('100%')
        .justifyContent(FlexAlign.End)
        .padding({ right: 12 })
        .position({ right: 0, top: 0 })
        .onTouch((event: TouchEvent) => event.stopPropagation())
      }
    }
    .width('100%')
    .height(this.barHeight)
    .backgroundColor(this.frosted ? Color.Transparent : $r('sys.color.background_secondary'))
    .onTouch((event: TouchEvent) => {
      // Start the system move only once the pointer travels, as the window
      // docs advise for click-and-drag: starting it on Down makes a plain
      // click restore a full-screen or maximized window.
      const point = event.touches[0];
      if (!point) return;
      if (event.type === TouchType.Down) {
        this.pressX = point.windowX;
        this.pressY = point.windowY;
        this.moving = false;
      } else if (event.type === TouchType.Move && !this.moving &&
        Math.hypot(point.windowX - this.pressX, point.windowY - this.pressY) > DRAG_THRESHOLD_VP) {
        this.moving = true;
        this.startMoving();
      }
    })
    .gesture(TapGesture({ count: 2 }).onAction(() => this.toggleMaximized()))
  }
}
