// Desk stand for the Waveshare ESP32-P4-WIFI6-Touch-LCD-3.4C, screwed to the
// four threaded mounting holes on the back of the display. Nothing wraps round
// the screen: an X-shaped bracket on short posts, one leg, and a flat base.
// The display leans back by `tilt`, with its bottom edge `rim_clear` above the
// desk so the USB-C cable can bend down; the cable then runs in a groove in the
// base and out of the back.
//
// Print base down, no supports. Screws: 4 x M3 (check: see hole_thread), long
// enough to pass the bracket and post and bite ~4 mm: post_h + bracket_t + 4.
//
// MEASURE BEFORE PRINTING. From Waveshare's drawing the large holes are 49.0 mm
// apart vertically (dimensioned) and about 59 mm apart horizontally, with the
// pattern's centre about 10 mm toward the USB-C side of the board's centre
// (both scaled from the picture, so approximate). Thread size isn't given.
//
// Coordinates for the hole pattern: as seen from the BACK of the display with
// the board upright (USB-C ports along the top), x right, y up, relative to the
// centre of the round display.

/* [Display] */
display_d = 115.0;
display_depth = 15.0;       // glass front to the tallest part on the back
board_back_z = 9.1;         // glass front to the surface the holes are in (board back)

/* [Mounting holes] */
hole_dx = 59.0;             // horizontal centre-to-centre (approximate: measure)
hole_dy = 49.0;             // vertical centre-to-centre
hole_center = [10.5, 1.5];  // pattern centre relative to the display centre (approximate)
hole_thread = 3.0;          // M3 assumed; 2.5 for M2.5
// 180 = board upside down, USB-C at the bottom and the cable hanging down
// (the firmware default, CONFIG_AIS_DISPLAY_UPSIDE_DOWN); 0 = USB-C at the top.
board_rotation = 180;
usb_x = 26.9;               // "USB to UART" port, x along the board's top edge (upright, seen from the back)

/* [Stand] */
tilt = 15;                  // degrees the screen leans back from vertical
rim_clear = 20;             // display's lowest point above the base: room for the cable to bend
post_d = 7;
post_clear = 1.0;           // posts reach this far past the tallest component
bracket_t = 4;
bar_w = 10;
leg_w = 30;
leg_t = 8;
base_w = 110;
base_d = 95;
base_t = 5;
cable_d = 5.5;              // groove for the USB cable

$fn = 64;

// Derived
R = display_d / 2;
post_h = display_depth + post_clear - board_back_z;   // board surface to bracket
bz0 = display_depth + post_clear;                     // bracket front face (display coordinates)
ts = sin(tilt);
tc = cos(tilt);
cz = base_t + rim_clear + R * tc + display_depth * ts;  // display centre height
eps = 0.01;

// Mounting holes in display coordinates (seen from the back), turned with the board.
function rot2(p, a) = [p[0] * cos(a) - p[1] * sin(a), p[0] * sin(a) + p[1] * cos(a)];
holes = [for (sx = [-1, 1], sy = [-1, 1])
    rot2([hole_center[0] + sx * hole_dx / 2, hole_center[1] + sy * hole_dy / 2], board_rotation)];
hub = rot2(hole_center, board_rotation);

// Display coordinates -> stand coordinates (z up, the viewer in front at -y).
// The display's x as seen from the back is the viewer's left, -x here.
function to_stand(p) = [-p[0], p[1] * ts + p[2] * tc, cz + p[1] * tc - p[2] * ts];
module on_stand() multmatrix([[-1, 0, 0, 0], [0, ts, tc, 0], [0, tc, -ts, cz], [0, 0, 0, 1]]) children();

// Bracket, in display coordinates: posts on the holes, X bars, a hub for the leg.
module bracket() {
    difference() {
        union() {
            for (h = holes) translate([h[0], h[1], board_back_z]) cylinder(d = post_d, h = post_h + bracket_t);
            for (i = [0, 1]) hull() {
                translate([holes[i][0], holes[i][1], bz0]) cylinder(d = bar_w, h = bracket_t);
                translate([holes[3 - i][0], holes[3 - i][1], bz0]) cylinder(d = bar_w, h = bracket_t);
            }
        }
        for (h = holes) translate([h[0], h[1], board_back_z - eps]) {
            cylinder(d = hole_thread + 0.4, h = 100);
            // counterbore for the screw head, from the back
            translate([0, 0, post_h + bracket_t - 2]) cylinder(d = hole_thread * 2 + 0.8, h = 10);
        }
    }
}

module stand() {
    hub_s = to_stand([hub[0], hub[1], bz0 + bracket_t / 2]);
    base_front = -R * ts - 15;
    leg_foot_y = hub_s[1] + 12;
    difference() {
        union() {
            on_stand() bracket();
            // leg: from the back of the hub down to the base
            hull() {
                on_stand() translate([hub[0] - leg_w / 2, hub[1] - 12, bz0]) cube([leg_w, 24, bracket_t]);
                translate([-leg_w / 2, leg_foot_y, base_t - eps]) cube([leg_w, leg_t + 20, 1]);
            }
            translate([-base_w / 2, base_front, 0]) hull()
                for (x = [8, base_w - 8], y = [8, base_d - 8]) translate([x, y, 0]) cylinder(r = 8, h = base_t);
        }
        // screw access from behind: keep the leg clear of the counterbores
        on_stand() for (h = holes) translate([h[0], h[1], bz0 + bracket_t - 2]) cylinder(d = hole_thread * 2 + 0.8, h = 40);
        // USB cable: drops behind the display's bottom edge, runs out of the back
        usb_p = rot2([usb_x, R], board_rotation);   // where the plug's cable leaves the display's edge
        if (usb_p[1] < 0) {
            c = to_stand([usb_p[0], usb_p[1], display_depth * 0.8]);
            translate([c[0] - cable_d / 2, c[1] - 10, base_t - cable_d * 0.6]) cube([cable_d, base_d, 20]);
        }
    }
}

// Rough stand-in for the display, for checking the fit.
module ghost_display() {
    color("black", 0.8) cylinder(r = R, h = 6);
    color("royalblue", 0.8) translate([0, 0, 6]) rotate(board_rotation) translate([-42.75, -32.5, 1.5]) cube([85.5, 65, board_back_z - 7.5]);
    color("silver", 0.8) translate([0, 0, board_back_z]) cylinder(r = 40, h = display_depth - board_back_z);
}

part = "stand";             // "stand", "assembly"
if (part == "stand") stand();
else { color("tan") stand(); on_stand() ghost_display(); }
