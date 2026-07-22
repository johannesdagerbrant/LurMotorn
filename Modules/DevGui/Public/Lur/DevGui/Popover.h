#pragma once
// Lur::DevGui::PlaceBelowOrAbove — vertical placement for a popover anchored to a row (the
// console's numpad + tooltip toaster both use it). Prefer BELOW the anchor row; flip ABOVE
// when the popover would run off the bottom of the screen; if it fits neither side cleanly,
// pick the side with more room and clamp fully on-screen. Pure geometry, host-testable.
namespace Lur::DevGui {

// Ay/Ah = anchor row top + height. Ph = popover height. Gap = px between row and popover.
// ScreenH = usable height. Returns the popover's top Y, always within [0, ScreenH-Ph]
// (or 0 when the popover is taller than the screen).
inline float PlaceBelowOrAbove(float Ay, float Ah, float Ph, float Gap, float ScreenH) {
    const float Below = Ay + Ah + Gap;
    if (Below + Ph <= ScreenH) return Below;         // fits below the row
    const float Above = Ay - Gap - Ph;
    if (Above >= 0.0f) return Above;                 // fits above the row
    // Neither side fits cleanly: take the roomier side, then clamp on-screen.
    const float RoomAbove = Ay;
    const float RoomBelow = ScreenH - (Ay + Ah);
    float Y = (RoomAbove > RoomBelow) ? Above : Below;
    if (Y + Ph > ScreenH) Y = ScreenH - Ph;
    if (Y < 0.0f) Y = 0.0f;
    return Y;
}

}  // namespace Lur::DevGui
