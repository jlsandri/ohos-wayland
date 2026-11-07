import { display } from '@kit.ArkUI';
import { deviceInfo } from '@kit.BasicServicesKit';

// A rectangle clears a display corner of radius R when both sides at that corner are inset by
// R × (1 − 1/√2).
const CLEAR_FRACTION = 1 - Math.SQRT1_2;

export interface CornerInsets {
  top: number;
  right: number;
  bottom: number;
  left: number;
}

export const NO_INSETS: CornerInsets = { top: 0, right: 0, bottom: 0, left: 0 };

export interface DisplayCorners {
  insets: CornerInsets;
  // The app area's corners are drawn round at the panel's radius. A display showing the top
  // part of a panel, above the Fold's keyboard, has square bottom corners, and this makes them
  // match the top; over a physically round corner it changes nothing.
  drawnRadius: number;
}

export const NO_CORNERS: DisplayCorners = { insets: NO_INSETS, drawnRadius: 0 };

// The inset in vp at each rounded panel corner, read with the `corner-ruler` calibration, for
// devices whose display reports no rounded corners (getRoundedCorner() returns none on the
// MateBook Fold). The ruler showed the Fold's top corners clear at 8 vp.
const MEASURED_INSETS: Record<string, number> = {
  'HPR-W72': 8,
};

function fromReportedCorners(corners: display.RoundedCorner[], density: number): CornerInsets {
  const clear = (type: display.CornerType): number => {
    const corner = corners.find((candidate: display.RoundedCorner) => candidate.type === type);
    return corner ? Math.ceil(corner.radius * CLEAR_FRACTION / density) : 0;
  };
  const topLeft = clear(display.CornerType.TOP_LEFT);
  const topRight = clear(display.CornerType.TOP_RIGHT);
  const bottomRight = clear(display.CornerType.BOTTOM_RIGHT);
  const bottomLeft = clear(display.CornerType.BOTTOM_LEFT);
  return {
    top: Math.max(topLeft, topRight),
    right: Math.max(topRight, bottomRight),
    bottom: Math.max(bottomLeft, bottomRight),
    left: Math.max(topLeft, bottomLeft),
  };
}

// The insets that keep a window filling the screen clear of the display's rounded corners.
export function displayCorners(density: number): DisplayCorners {
  try {
    const corners = display.getDefaultDisplaySync().getRoundedCorner();
    if (corners.length > 0) return { insets: fromReportedCorners(corners, density), drawnRadius: 0 };
  } catch (error) {
    console.warn(`DISPLAY_CORNERS ${JSON.stringify(error)}`);
  }
  const inset = MEASURED_INSETS[deviceInfo.productModel.trim()] ?? 0;
  if (inset === 0) return NO_CORNERS;
  return { insets: { top: inset, right: inset, bottom: inset, left: inset }, drawnRadius: Math.round(inset / CLEAR_FRACTION) };
}
