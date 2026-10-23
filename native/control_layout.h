#pragma once

// One logical-DPI grid for painting, hit testing, tooltips and UI Automation.
namespace control_layout {
constexpr int rail_width=100, height=216, panel_top=10, panel_bottom=206;
constexpr int row_step=44, row_x=38, button_left=16, button_right=60;
constexpr int arrow_y=(panel_top+panel_bottom)/2, arrow_half_width=12, arrow_half_height=18;
constexpr int arrow_top=arrow_y-arrow_half_height, arrow_bottom=arrow_y+arrow_half_height;
constexpr int row_center(int index){return 40+index*row_step;}
constexpr int row_top(int index){return row_center(index)-22;}
constexpr int row_bottom(int index){return row_center(index)+22;}
}
