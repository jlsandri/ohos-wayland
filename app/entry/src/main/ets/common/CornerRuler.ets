// Calibrates a display's rounded corners (Index.ets `corner-ruler`). Dots sit on each corner's
// diagonal at 0, 2, 4 ... vp from the corner. A corner of radius R hides the dots closer than
// R × (1 − 1/√2), so the first visible dot is the inset that keeps a rectangle clear of it.
const STEP = 2;
const COUNT = 21;
const DOT = 2;

interface Corner {
  alignment: Alignment;
  // Direction from the corner into the screen.
  x: number;
  y: number;
}

const CORNERS: Corner[] = [
  { alignment: Alignment.TopStart, x: 1, y: 1 },
  { alignment: Alignment.TopEnd, x: -1, y: 1 },
  { alignment: Alignment.BottomStart, x: 1, y: -1 },
  { alignment: Alignment.BottomEnd, x: -1, y: -1 },
];

const DISTANCES: number[] = Array.from<number, number>({ length: COUNT }, (_: number, i: number) => i * STEP);

@Builder
function cornerDots(corner: Corner) {
  Stack({ alignContent: corner.alignment }) {
    ForEach(DISTANCES, (d: number) => {
      Circle()
        .width(DOT)
        .height(DOT)
        .fill(d % 10 === 0 ? '#FF9F0A' : '#FAFAFA')
        .translate({ x: corner.x * d, y: corner.y * d })
      if (d % 10 === 0 && d > 0) {
        Text(`${d}`)
          .fontSize(9)
          .fontColor('#FF9F0A')
          .translate({ x: corner.x * (d + 4), y: corner.y * (d - 8) })
      }
    }, (d: number) => `${d}`)
  }
  .width('100%')
  .height('100%')
}

@Builder
export function CornerRuler() {
  Stack() {
    ForEach(CORNERS, (corner: Corner) => {
      cornerDots(corner)
    }, (corner: Corner) => `${corner.x}|${corner.y}`)
  }
  .width('100%')
  .height('100%')
  .backgroundColor('#000000')
  .hitTestBehavior(HitTestMode.None)
}
