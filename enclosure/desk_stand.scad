// Desk stand for the Waveshare ESP32-P4-WIFI6-Touch-LCD-3.4C, screwed to the
// four M4 threaded holes in the back of the display (a 75 x 75 mm square,
// centred: the VESA 75 pattern). Nothing wraps round the screen: an X-shaped
// bracket on four posts, one leg, and a flat base. The display leans back by
// `tilt`, with its bottom edge `rim_clear` above the desk so the USB-C cable can
// bend down; the cable then runs in a groove in the base and out of the back.
//
// Print base down, no supports. Screws: 4 x M4, long enough to pass the
// bracket and post and bite `thread_engage` mm: post_h + bracket_t + thread_engage
// (16 mm with the defaults). Check how deep the threaded holes are first, so
// the screws can't bottom out against the display.
//
// Display coordinates: seen from the BACK, x right, y up, from the centre of the
// round display, with the board's USB-C edge at the bottom (the firmware's
// default, CONFIG_AIS_DISPLAY_UPSIDE_DOWN). Dimensions are from Waveshare's
// drawing of the display back.

/* [Display] */
display_d = 115.0;
display_depth = 15.0;       // glass front to the tallest part on the back
plate_z = 6.0;              // glass front to the back plate the M4 holes are in
board_w = 85.0;             // board, along the USB-C edge
board_h = 64.7;
board_y0 = -28.1;           // the board's USB-C edge (bottom), from the display centre
board_x0 = -42.5;

/* [Mounting holes] */
hole_spacing = 75.0;        // square pattern, centre to centre
hole_thread = 4.0;          // M4
thread_engage = 4;          // how far the screws go into the threaded holes

/* [Cable] */
usb_x = -26.7;              // "USB to UART" port along the bottom edge (seen from the back;
                            // 26.7 mm right of centre seen from the front). Native USB: -12.3.

/* [Stand] */
tilt = 15;                  // degrees the screen leans back from vertical
rim_clear = 20;             // display's lowest point above the base: room for the cable to bend
post_d = 7;                 // fits the ring round each hole on the display back
post_clear = 1.0;           // the bracket clears the tallest component by this much
bracket_t = 4;
bar_w = 10;
leg_w = 24;
leg_t = 8;
base_w = 110;
base_d = 95;
base_t = 5;
cable_d = 5.5;              // groove for the USB cable

$fn = 64;

// Derived
R = display_d / 2;
bz0 = display_depth + post_clear;                     // bracket front face
post_h = bz0 - plate_z;
ts = sin(tilt);
tc = cos(tilt);
cz = base_t + rim_clear + R * tc + display_depth * ts;  // display centre height
eps = 0.01;
holes = [for (sx = [-1, 1], sy = [-1, 1]) [sx * hole_spacing / 2, sy * hole_spacing / 2]];

// Display coordinates -> stand coordinates (z up, the viewer in front at -y).
// The display's x as seen from the back is the viewer's left, -x here.
function to_stand(p) = [-p[0], p[1] * ts + p[2] * tc, cz + p[1] * tc - p[2] * ts];
module on_stand() multmatrix([[-1, 0, 0, 0], [0, ts, tc, 0], [0, tc, -ts, cz], [0, 0, 0, 1]]) children();

// Bracket, in display coordinates: posts on the holes, X bars across them.
module bracket() {
    difference() {
        union() {
            for (h = holes) translate([h[0], h[1], plate_z]) cylinder(d = post_d, h = post_h + bracket_t);
            for (i = [0, 1]) hull() {
                translate([holes[i][0], holes[i][1], bz0]) cylinder(d = bar_w, h = bracket_t);
                translate([holes[3 - i][0], holes[3 - i][1], bz0]) cylinder(d = bar_w, h = bracket_t);
            }
        }
        for (h = holes) translate([h[0], h[1], plate_z - eps]) {
            cylinder(d = hole_thread + 0.4, h = 100);
            // counterbore for the screw head, from the back
            translate([0, 0, post_h + bracket_t - 2.5]) cylinder(d = hole_thread * 2 + 1, h = 10);
        }
    }
}

module stand() {
    hub_s = to_stand([0, 0, bz0 + bracket_t / 2]);
    base_front = -R * ts - 15;
    difference() {
        union() {
            on_stand() bracket();
            // leg: from the back of the bracket's centre straight down to the base
            hull() {
                on_stand() translate([-leg_w / 2, -12, bz0]) cube([leg_w, 24, bracket_t]);
                translate([-leg_w / 2, hub_s[1] + 12, base_t - eps]) cube([leg_w, leg_t + 20, 1]);
            }
            translate([-base_w / 2, base_front, 0]) hull()
                for (x = [8, base_w - 8], y = [8, base_d - 8]) translate([x, y, 0]) cylinder(r = 8, h = base_t);
        }
        // keep the screw heads reachable from behind
        on_stand() for (h = holes) translate([h[0], h[1], bz0 + bracket_t - 2.5]) cylinder(d = hole_thread * 2 + 1, h = 40);
        // USB cable: drops behind the display's bottom edge, runs out of the back
        c = to_stand([usb_x, -R, display_depth * 0.8]);
        translate([c[0] - cable_d / 2, c[1] - 10, base_t - cable_d * 0.6]) cube([cable_d, base_d, 20]);
    }
}

// Rough stand-in for the display, for checking the fit: glass, board and the
// space its components take, with the board's notches round the mounting holes.
module ghost_display() {
    color("black", 0.8) cylinder(r = R, h = plate_z);
    color("royalblue", 0.8) difference() {
        translate([board_x0, board_y0, plate_z + 1.6]) cube([board_w, board_h, display_depth - plate_z - 1.6]);
        for (h = holes) translate([h[0], h[1], 0]) cylinder(d = post_d + 2, h = 50);
    }
}

part = "stand";             // "stand", "assembly"
if (part == "stand") stand();
else if (part == "assembly") { color("tan") stand(); on_stand() ghost_display(); }
