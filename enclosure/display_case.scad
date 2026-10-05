// Round case for the Waveshare ESP32-P4-WIFI6-Touch-LCD-3.4C (800x800 round display).
//
// Two printed parts:
//   shell : front bezel + side wall. The display drops in from the back, glass
//           first, and sits against a lip that covers its black border.
//   lid   : back cover. A ring on it clamps the display's rim against the lip.
//           Three M3 screws through lugs on the outside of the wall hold it on.
//           Keyhole slots for hanging it on two screws, and vent slots.
// The board is mounted upside down so its USB-C ports face down and the cable
// hangs straight out of a notch in the bottom of the wall, which the lid closes
// from behind. The firmware turns the picture and touch round to match
// (CONFIG_AIS_DISPLAY_UPSIDE_DOWN, on by default). The microSD card is reached
// by taking the lid off.
//
// Coordinates: looking at the screen, x right, y up; z = 0 at the front of the
// case, increasing toward the back.
//
// Dimensions are from Waveshare's drawings (docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-XC):
// display module Ø115 x 6.0 mm, active area Ø87.6, 15.0 mm from the glass to
// the tallest part on the back; board 85.5 x 65 mm. The board is assumed to be
// centred on the display. Upright (camera connector at the top) its two USB-C
// ports are on the top edge, left of centre; upside down they're on the bottom
// edge, right of centre. Check the notch position with a test print before
// printing in your final colour.
//
// Printing: no supports. Shell front face down; lid outside face down.
// Screws: 3 x M3 x 10 countersunk, self-tapping into the 2.6 mm pilot holes
// (or widen them to ~4.0 mm for heat-set inserts).

part = "assembly";      // "shell", "lid", "assembly" (with a stand-in display), "section"

/* [Display] */
panel_d = 115.0;        // glass / display module diameter
panel_t = 6.0;          // display module thickness
active_d = 87.6;        // visible area
total_depth = 15.0;     // glass front to the tallest part on the back of the board
board_w = 85.5;
board_h = 65.0;

/* [Case] */
fit = 0.35;             // radial clearance around the display
wall = 2.4;             // side wall thickness
lip_t = 1.6;            // front lip thickness
lip_margin = 1.0;       // the lip stops this far outside the active area
back_gap = 2.5;         // free space behind the tallest component
lid_t = 2.4;
clamp_ring_w = 2.4;     // ring on the lid that presses the display rim forward
clamp_preload = 0.3;    // ring is this much longer than the gap, to hold the display firmly
lug_angles = [90, 210, 330];      // screw lugs: top, lower left, lower right (clear of the USB-C notch)
lug_d = 9.0;

/* [Openings] */
// 180 = board upside down, USB-C ports at the bottom and the cable hanging down
// (matches the firmware default); 0 = ports at the top, cable out of the top.
board_rotation = 180;
// USB-C ports along the board's top edge (board upright): x of each connector
// centre, looking at the screen. -26.9 = "USB to UART" (flashing, serial log),
// -12.4 = native USB. Either one powers the board. One notch per port listed.
usb_ports_x = [-26.9];
usb_notch_w = 13.5;     // wide enough for a straight USB-C plug's overmould
usb_notch_z = 9.0;      // the notch runs from here to the back of the wall
keyholes = true;        // two keyhole slots in the lid for hanging on screws
keyhole_spacing = 60;
vents = true;

$fn = 160;

// Derived
R = panel_d / 2 + fit;              // inside radius of the wall
Ro = R + wall;                      // outside radius
lug_r = Ro + 1.6;                   // radius of the screw centres
lip_inner_r = active_d / 2 + lip_margin;
z_panel_back = lip_t + panel_t;
z_back = lip_t + total_depth + back_gap;   // back edge of the wall; the lid sits here
eps = 0.01;

// Notch cutters for the USB-C ports, turned with the board.
module usb_notches(z0, w = usb_notch_w) {
    rotate(board_rotation)
        for (x = usb_ports_x) translate([x - w / 2, 0, z0]) cube([w, Ro + 1, z_back + 100]);
}

module lug_outline(h) {
    for (a = lug_angles) rotate(a)
        hull() {
            translate([lug_r, 0, 0]) cylinder(d = lug_d, h = h, $fn = 48);
            translate([R, -lug_d / 2, 0]) cube([1, lug_d, h]);
        }
}

module shell() {
    difference() {
        union() {
            cylinder(r = Ro, h = z_back);
            // screw lugs, from part way back so the front stays a clean circle;
            // the 45° underside prints without support
            for (a = lug_angles) rotate(a)
                hull() {
                    translate([lug_r, 0, z_back - 8]) cylinder(d = lug_d, h = 8, $fn = 48);
                    translate([Ro - 1, -lug_d / 2, z_back - 8 - (lug_r + lug_d / 2 - Ro + 1)]) cube([1, lug_d, 1]);
                }
        }
        // display pocket and the cavity behind it
        translate([0, 0, lip_t]) cylinder(r = R, h = z_back);
        // window, chamfered so the lip doesn't shade the edge of the screen
        translate([0, 0, -eps]) cylinder(r1 = lip_inner_r + lip_t, r2 = lip_inner_r, h = lip_t + 2 * eps);
        // M3 pilot holes
        for (a = lug_angles) rotate(a) translate([lug_r, 0, z_back - 7]) cylinder(d = 2.6, h = 10, $fn = 32);
        // USB-C notches, open at the back so the lid closes them
        usb_notches(usb_notch_z);
    }
}

// The lid as it sits on the case, outside face at z = 0 and the ring pointing
// toward +z. lid() mirrors it in x, because it is turned over to fit.
module lid_installed() {
    ring_h = lid_t + (z_back - z_panel_back) + clamp_preload;
    difference() {
        union() {
            cylinder(r = Ro, h = lid_t);
            lug_outline(lid_t);
            // clamp ring, also locates the lid in the wall; stays outside the
            // board, whose corners reach r = 53.7
            difference() {
                cylinder(r = R - 0.3, h = ring_h);
                translate([0, 0, -eps]) cylinder(r = R - 0.3 - clamp_ring_w, h = 100);
                usb_notches(-eps);
            }
        }
        // countersunk screw holes
        for (a = lug_angles) rotate(a) translate([lug_r, 0, -eps]) {
            cylinder(d = 3.4, h = 100, $fn = 32);
            cylinder(d1 = 6.6, d2 = 3.4, h = 1.6, $fn = 32);
        }
        if (vents)
            for (i = [-3:3]) translate([i * 9 - 2, -30, -eps]) cube([4, 26, lid_t + 2]);
        if (keyholes)
            for (s = [-1, 1]) translate([s * keyhole_spacing / 2, 20, -eps]) keyhole();
    }
}

module lid() { mirror([1, 0, 0]) lid_installed(); }

// A screw head goes in the big hole; the case then drops so the shank sits at
// the top of the slot.
module keyhole() {
    cylinder(d = 8.5, h = 100, $fn = 48);
    translate([-2.25, 0, 0]) cube([4.5, 9, 100]);
    translate([0, 9, 0]) cylinder(d = 4.5, h = 100, $fn = 32);
}

// Rough stand-in for the display, for checking the fit.
module ghost_display() {
    color("black", 0.9) translate([0, 0, lip_t]) cylinder(r = panel_d / 2, h = panel_t);
    rotate(board_rotation) ghost_board();
}

module ghost_board() {
    color("royalblue", 0.9) translate([-board_w / 2, -board_h / 2, z_panel_back + 1.5]) cube([board_w, board_h, 1.6]);
    // the tallest parts: USB-A sticking out of the right side, USB-C on top
    top = lip_t + total_depth;
    color("silver") {
        translate([board_w / 2 - 8, -7, top - 6.4]) cube([8 + 6.6, 14, 6.4]);
        for (x = [-26.9, -12.4]) translate([x - 4.5, board_h / 2 - 7.5, z_panel_back + 3.1]) cube([9, 7.5, 3.3]);
    }
}

module assembly() {
    color("dimgray") shell();
    ghost_display();
    color("silver", 0.7) translate([0, 0, z_back + lid_t]) rotate([0, 180, 0]) lid();
}

if (part == "shell") shell();
else if (part == "lid") lid();
else if (part == "section") difference() { assembly(); translate([0, -200, -50]) cube([200, 400, 200]); }
else if (part == "assembly") assembly();
