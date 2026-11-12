// ESP32 + Velleman VMA432 DMX node: parametric enclosure (base + lid + test coupon)
// OpenSCAD 2021.01 or newer. All sizes in mm.
//
// Export (run in this folder, bash):
//   openscad -o base.stl   -D 'part="base"'   -D show_assembly=false enclosure.scad
//   openscad -o lid.stl    -D 'part="lid"'    -D show_assembly=false enclosure.scad
//   openscad -o coupon.stl -D 'part="coupon"' -D show_assembly=false enclosure.scad
// The STLs come out in print orientation (base floor down, lid outer face down).
//
// Every value marked TODO is a guess. Measure it, then print coupon.stl before the full box.
// Defaults for the ESP32 buttons come from PLAUK's "ESP32-WROOM-32 (USB-C) Box" on Printables (CC BY 4.0).
//
// Layout, seen from above (USB end at the bottom):
//
//        wall B (XLR)
//   +---------------------+
//   | XLR  |   antenna    |
//   |VMA432|              |      ESP32 lies component side up on two ribs, pins and Dupont plugs hanging below.
//   |      |    ESP32     |      VMA432: the Neutrik nose goes through wall B (flush outside), the flange rests on
//   |dupont|              |      the inside. A lid bracket behind the XLR takes the push of a plug. Flange screws optional.
//   |      |              |      The space under the ESP32 is open, so the three wires can run (and coil) there.
//   +------+--[USB-C]-----+
//        wall A (USB)
//   +y <---              (x runs from wall A to wall B, y from the ESP32 wall to the VMA432 wall)
//
// Coordinates: origin at the inside corner of wall A and the ESP32-side wall, z = 0 on top of the floor.

/* [Output] */
part          = "assembly"; // [assembly, base, lid, coupon]
show_assembly = true;       // ghost ESP32 + VMA432 (assembly and base views; never exported)
explode       = 0;          // lift the lid in the assembly view
cut_y         = -1;         // >= 0: cut the assembly view at this y to look inside

/* [Print and fit] */
wall    = 2.0;
floor_t = 2.0;
lid_t   = 2.0;
clr     = 0.4;  // gap on every fit surface, per side (on the loose side: a bit big still fits, too small doesn't)
edge_r  = 1.0;  // fillet on vertical outside edges
bed_ch  = 1.0;  // 45 deg chamfer on edges that touch the print bed (a fillet there would droop)

/* [ESP32 dev board] */
esp_l            = 48.5;  // measured
esp_w            = 25.0;  // measured
esp_pcb_t        = 1.6;   // TODO PCB thickness
esp_top_h        = 4.0;   // TODO tallest part on top of the PCB (module can is about 3.3)
esp_under        = 28.5;  // under the PCB: 8.5 pins + 20 Dupont plugs and wire bend (measured)
esp_usb_overhang = 2.0;   // measured: USB-C shell sticks out past the PCB edge, into the wall
esp_usb_dy       = 0.0;   // measured: centred
esp_usb_z        = 1.63;  // TODO USB-C centre height above the PCB top (1.63 = shell sits on top of the PCB)
usb_shell_w      = 8.94;  // standard USB-C receptacle shell
usb_shell_h      = 3.26;
usb_recess_w     = 14.0;  // recess on the outside, so a plug's overmold can seat fully
usb_recess_h     = 8.5;
usb_recess       = 1.0;   // recess depth; the port sits in the remaining wall
// Buttons: [distance from the USB-end PCB edge, offset from the centre line (+ = toward the VMA432)].
// Looking down with the USB toward you, EN is on the left (+y) and BOOT (IO0) on the right.
esp_btn_boot     = [2.5, -7.4]; // measured from the edge; side offset from the photo
esp_btn_en       = [3.0,  7.4];
esp_btn_h        = 2.5;   // TODO button top above the PCB top
esp_shield_cx    = 16.0;  // centre of the module's metal can from the antenna-end PCB edge (from the photo)
esp_shield_h     = 3.3;   // TODO top of the metal can above the PCB top
esp_row_pitch    = 22.86; // header rows, centre to centre (ghost only)
esp_row_pins     = 19;    // 38-pin board (ghost only)

/* [Velleman VMA432] */
vma_l          = 40.0;  // measured
vma_w          = 20.0;  // measured
vma_pcb_t      = 1.6;   // measured (the XLR hole height follows it)
vma_h          = 32.0;  // measured: PCB bottom to the top of the metal PUSH latch
vma_standoff   = 2.5;   // gap under the PCB: the XLR solder joints stick out 2 mm
vma_dupont_len = 15.0;  // measured: Dupont plugs past the header end of the PCB
vma_edge_back  = 0.0;   // measured: PCB front edge flush with the XLR nose (2.7 would be at the flange)
xlr_dy         = 0.0;   // measured: XLR centred on the PCB
xlr_latch_w    = 10.0;  // measured: widest part of the metal PUSH latch
xlr_latch_out  = 4.0;   // measured: latch sticks out in front of the nose (ghost only)
/* [Neutrik NC3FAH1-0, from datasheet ST-NC3FAH1-0] */
xlr_nose_d   = 21.9;  // round front that goes through the wall
xlr_nose     = 2.7;   // its length in front of the flange
xlr_body     = 19.5;  // flange to the rear of the housing
xlr_cz       = 12.5;  // centre above the PCB top (housing seating plane)
xlr_up       = 12.8;  // flange edges from the centre, seen from the front
xlr_left     = 12.8;  //   (left = the side toward the ESP32)
xlr_right    = 12.5;
xlr_ear      = 9.9;   // flange screw holes at (-9.9, +9.9) and (+9.9, -9.9), seen from the front
xlr_cutout_d = 22.0;  // Neutrik panel cut-out
xlr_screw_d  = 3.2;   // Neutrik panel holes for the flange screws
xlr_screws   = true;  // drill wall B for the two flange screws (optional; the lid bracket holds it anyway)

/* [Layout] */
wire_bend = 12.0; // free room past the VMA432 Dupont plugs, so the wires and pins don't get bent hard
bay_gap   = 0.5;  // between the ESP32 guides and the XLR housing
guide_t   = 1.6;  // guide and stop thickness
top_gap   = 1.0;  // VMA432 latch top to the lid

/* [Lid and screws] */
screw_d      = 3.4;    // M3 clearance in the lid
pilot_d      = 2.5;    // M3 thread-forming pilot in the bosses (2.6 for PETG)
pilot_depth  = 10.0;   // use M3 x 10 screws
tower_r      = 3.5;    // corner screw bosses (the lid ears get no chamfer: it ate them on the first print)
lid_ch       = 0.4;    // small chamfer on the lid's outer edge (printed on the bed)
screw_head   = "pan";  // [pan, countersunk]
cs_d         = 6.4;    // countersink diameter (screw_head = "countersunk")
lip_t        = 1.2;    // lid lip inside the walls
lip_h        = 2.5;
// BOOT/EN: flexible tabs in the lid. A U-shaped cut leaves a thin tab hinged on the inner side;
// a nub under its free end presses the board's button when you press the tab with a finger.
tab_len      = 18.0;   // free end to hinge
tab_t        = 0.8;    // tab thickness (the rest of the lid is lid_t)
tab_slot     = 1.2;    // width of the U-shaped cut (0.8 partly closed up on the first layer)
nub_d        = 3.5;    // pin under the tab
nub_gap      = 1.0;    // nub end to button top; too small and the lid holds BOOT down (the firmware stop!)
// [button, label, tab width, tab centre shift from the button (+ = toward the VMA432)]
btn_tabs     = [[esp_btn_boot, "BOOT", 10, 1.0], [esp_btn_en, "EN", 8, 0]];
gauge_hole_d = 3.2;    // holes in the coupon's button gauge
label_size   = 2.8;
label_depth  = 0.3;    // shallow, so the thin tabs stay strong
label_font   = "Liberation Sans:style=Bold";
lid_text       = ["Schuwe", "Lichtoplossingen"];  // engraved on the lid, one entry per line
lid_text_size  = [8, 5.2];
lid_text_gap   = 2.5;  // between the lines
lid_text_depth = 1.0;  // deep, so it still reads on a textured print bed
holddown_d   = 4.0;    // lid posts that keep the boards down
holddown_gap = 0.8;    // their gap above the parts; must stay below nub_gap

/* [ESP32 clip] */
clip_x0  = 10.0;   // start of the snap beam on the VMA432 side, from the USB-end PCB edge
clip_len = 10.0;
clip_t   = 2.0;
clip_lip = 0.8;    // lip reaches this far over the board edge; the pins start 1-2 mm in
clip_set = 1.5;    // below the board the beam steps back this far, clear of the Dupont plugs
bump_t   = 0.3;    // bumps on the opposite wall; the board is clamped between them and the beam

/* [Vents and mounting] */
vent_n     = 4;
vent_len   = 8.0;
vent_h     = 2.0;      // slot height; slanted 45 deg so there is no straight view in
vent_pitch = 11.0;
vent_x0    = 12.0;
vent_rows  = [7, 14];  // z of each row, well below the ESP32 PCB
mount_ears = true;     // side ears with a Velcro strap slot and two screw holes each
ear_w      = 9.0;
ear_t      = 3.0;
ear_slot_l = 22.0;     // for a 20 mm strap
ear_slot_w = 3.5;
ear_hole_d = 3.5;

$fa = 4;
$fs = 0.4;

// ---------------------------------------------------------------- derived
esp_x0 = clr;                     // USB-end PCB edge rests on wall A; the USB-C shell goes into the wall
esp_x1 = esp_x0 + esp_l;
esp_y0 = clr;
esp_y1 = esp_y0 + esp_w;
esp_cy = (esp_y0 + esp_y1) / 2;
usb_y  = esp_cy + esp_usb_dy;

vma_stack = vma_standoff + vma_h + top_gap;
esp_stack = esp_under + esp_pcb_t + esp_usb_z + usb_recess_h / 2 + 1;   // 1 mm of wall above the recess
H_in      = max(vma_stack, esp_stack);                       // floor top to lid underside
esp_z0    = min(H_in - 1 - usb_recess_h / 2 - esp_usb_z - esp_pcb_t,  // ESP32 PCB bottom, as high as fits
                H_in - top_gap - esp_top_h - esp_pcb_t);
esp_top   = esp_z0 + esp_pcb_t;
usb_cz    = esp_top + esp_usb_z;
usb_slot_bottom = usb_cz - usb_shell_h / 2 - clr - 0.5;      // 0.5 extra in case the port sits a bit lower
usb_shell_top   = usb_cz + usb_shell_h / 2;

L_in     = max(esp_x1 + clr + guide_t + 1, wire_bend + vma_dupont_len + vma_l + vma_edge_back - wall);
face_x   = L_in + wall;              // XLR front face, flush with the outside of wall B
flange_x = face_x - xlr_nose;        // flange rests on the inside of wall B
pad_t    = xlr_nose - wall;          // wall B is thickened to the nose length around the XLR
vma_x1   = face_x - vma_edge_back;   // PCB front edge
vma_x0   = vma_x1 - vma_l;           // header end
xlr_y    = esp_y1 + clr + guide_t + bay_gap + xlr_left;
vma_cy   = xlr_y - xlr_dy;
W_in     = max(xlr_y + xlr_right, vma_cy + vma_w / 2) + clr;
vma_top  = vma_standoff + vma_pcb_t;
xlr_z    = vma_top + xlr_cz;
xlr_top_z  = xlr_z + xlr_up;
xlr_rear_x = flange_x - xlr_body;
xlr_hole_d   = xlr_cutout_d + 2 * clr;
latch_slot_w = xlr_latch_w + 2 * clr;
latch_top    = vma_standoff + vma_h;

tower_off = tower_r / sqrt(2) + 0.1;  // boss centre outside the cavity corner, so it never intrudes
tower_xy  = [[-tower_off, -tower_off], [L_in + tower_off, -tower_off],
             [-tower_off, W_in + tower_off], [L_in + tower_off, W_in + tower_off]];
ear_l     = ear_slot_l + 2 * (ear_hole_d + 5);

function btn_xy(b) = [esp_x0 + b[0], esp_cy + b[1]];

assert(esp_z0 >= esp_under, str("Only ", esp_z0, " mm under the ESP32, need esp_under = ", esp_under));
assert(esp_top + esp_top_h + clr <= H_in, "ESP32 top parts hit the lid");
assert(xlr_y + xlr_hole_d / 2 <= W_in - 0.5, "XLR hole runs into the side wall");
assert(xlr_z - xlr_hole_d / 2 >= 0.5, "XLR hole runs into the floor");
assert(usb_y - usb_recess_w / 2 >= 0.5, "USB recess runs into the side wall");
assert(esp_x0 - esp_usb_overhang <= -(wall - usb_recess) + 0.5, "USB-C port doesn't reach the recess: make usb_recess deeper");
assert(latch_top <= H_in, "XLR latch is taller than the box");
assert(pad_t >= 0, "wall is thicker than the XLR nose: the nose would not reach the outside");
assert(xlr_top_z + clr < H_in, "XLR flange hits the lid");
assert(vma_cy - vma_w / 2 - clr - guide_t > esp_y1 + clr + guide_t, "VMA432 guides hit the ESP32 guides");

echo(str("Inside: ", L_in, " x ", W_in, " x ", H_in, " mm"));
echo(str("Outside (walls): ", L_in + 2 * wall, " x ", W_in + 2 * wall, " x ", floor_t + H_in + lid_t, " mm"));
echo(str("Outside (incl. corner bosses", mount_ears ? " and ears" : "", "): ",
         L_in + 2 * (tower_off + tower_r), " x ",
         mount_ears ? W_in + 2 * (wall + ear_w) : W_in + 2 * (tower_off + tower_r), " mm"));
echo(str("Room under the ESP32 PCB: ", esp_z0, " mm"));
echo(str("BOOT/EN nubs: ", H_in + lid_t - tab_t - (esp_top + esp_btn_h + nub_gap), " mm long, ", nub_gap, " mm above the buttons"));

// ---------------------------------------------------------------- helpers
module rrect(size, r) { offset(r = r) offset(delta = -r) square(size); }

module outline_box() { translate([-wall, -wall]) rrect([L_in + 2 * wall, W_in + 2 * wall], edge_r); }

// Prism of a convex 2D child with a 45 deg chamfer at the bottom (or the top).
module chamfered_prism(h, ch, at_top = false) {
    hull() {
        translate([0, 0, at_top ? h - 0.01 : 0]) linear_extrude(0.01) offset(delta = -ch) children();
        translate([0, 0, at_top ? 0 : ch]) linear_extrude(h - ch) children();
    }
}

module chamfered_cylinder(r, h, ch, at_top = false) {
    if (at_top) {
        cylinder(r = r, h = h - ch);
        translate([0, 0, h - ch]) cylinder(r1 = r, r2 = r - ch, h = ch);
    } else {
        cylinder(r1 = r - ch, r2 = r, h = ch);
        translate([0, 0, ch]) cylinder(r = r, h = h - ch);
    }
}

// Extrude a 2D shape drawn in (y, z) along +x, starting at x0.
module yz_extrude(x0, len) {
    multmatrix([[0, 0, 1, x0], [1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 0, 1]]) linear_extrude(len) children();
}

// ---------------------------------------------------------------- base
// USB-C: a slot for the port through wall A, open to the top so the board drops straight in
// (the lid tongue closes it), and a recess on the outside for the plug's overmold.
// cz = port centre height, top = wall top.
module usb_cutout(cz, top) {
    bottom = cz - usb_shell_h / 2 - clr - 0.5;
    yz_extrude(-wall - 1, wall + 2)
        translate([usb_y - usb_shell_w / 2 - clr, bottom]) square([usb_shell_w + 2 * clr, top + 1 - bottom]);
    yz_extrude(-wall - 1, usb_recess + 1)
        translate([usb_y, cz]) offset(r = 2) offset(delta = -2) square([usb_recess_w, usb_recess_h], center = true);
}

module xlr_opening_2d() {
    r = xlr_hole_d / 2;
    translate([xlr_y, xlr_z]) {
        circle(r);
        // 45 deg shoulders, so the top of the hole prints without support
        polygon([[-r * cos(45), r * sin(45)], [r * cos(45), r * sin(45)], [0, r * sqrt(2)]]);
    }
    // slot for the metal latch, open to the top so the latch can come down into it
    translate([xlr_y - latch_slot_w / 2, xlr_z]) square([latch_slot_w, H_in + 1 - xlr_z]);
    // the PCB runs out to the XLR front, so it passes through the wall too
    if (vma_edge_back < xlr_nose)
        translate([vma_cy - vma_w / 2 - clr, vma_standoff - clr]) square([vma_w + 2 * clr, vma_pcb_t + 2 * clr]);
    if (xlr_screws)
        for (s = [-1, 1]) translate([xlr_y + s * xlr_ear, xlr_z - s * xlr_ear]) circle(d = xlr_screw_d);
}

// Wall B is thickened to the nose length behind the flange, so the nose ends flush with the outside.
module xlr_pad() {
    y0 = xlr_y - xlr_left - 1.5;
    y1 = min(W_in, xlr_y + xlr_right + 1.5);
    if (pad_t > 0) translate([flange_x, y0, 0]) cube([pad_t + 0.01, y1 - y0, xlr_top_z + 1]);
}

module vents() {
    for (z = vent_rows, i = [0 : vent_n - 1]) {
        x = vent_x0 + i * vent_pitch;
        // outer opening higher than the inner one: the view goes down to the floor, never to the board
        hull() {
            translate([x - vent_len / 2, -wall - 0.01, z + wall / 2 - vent_h / 2]) cube([vent_len, 0.01, vent_h]);
            translate([x - vent_len / 2, 0, z - wall / 2 - vent_h / 2]) cube([vent_len, 0.01, vent_h]);
        }
    }
}

module ear_2d(side) {
    // side 0 = ESP32 wall (y < 0), side 1 = VMA432 wall
    y = side == 0 ? -wall - ear_w : W_in - bed_ch - 1;
    translate([L_in / 2 - ear_l / 2, y]) rrect([ear_l, ear_w + wall + bed_ch + 1], edge_r);
}

module ear_cuts(side) {
    yc = side == 0 ? -wall - ear_w / 2 : W_in + wall + ear_w / 2;
    translate([0, 0, -floor_t - 1]) linear_extrude(ear_t + 2) {
        hull() for (s = [-1, 1]) translate([L_in / 2 + s * (ear_slot_l - ear_slot_w) / 2, yc]) circle(d = ear_slot_w);
        for (s = [-1, 1]) translate([L_in / 2 + s * (ear_slot_l / 2 + 2.5 + ear_hole_d / 2), yc]) circle(d = ear_hole_d);
    }
}

module esp_supports() {
    rib_half = 8.5;
    rib_t = 2.0;
    // two ribs under the middle of the PCB, clear of the header rows; the first one sits under the buttons
    for (x = [esp_x0 + esp_btn_boot[0], esp_x1 - 10])
        translate([x - rib_t / 2, esp_cy - rib_half, 0]) cube([rib_t, 2 * rib_half, esp_z0]);
    gz = esp_top + 1.5;
    sx = esp_x1 + clr;
    // antenna-end stops at the two corners, outside the module width; they take the push of a USB plug
    translate([sx, 0, 0]) cube([guide_t, esp_y0 + 3, gz]);
    translate([sx, esp_y1 - 3, 0]) cube([guide_t, 3 + clr + guide_t, gz]);
    // side guide toward the VMA432 at the antenna end; it runs on to wall B, so the stop can't bend back
    translate([sx + guide_t - 6, esp_y1 + clr, 0]) cube([L_in - (sx + guide_t - 6) + 0.01, guide_t, gz]);
    esp_clip();
    // bumps on the ESP32-side wall at board level; with the clip they clamp the board in place
    for (x = [esp_x0 + 12, esp_x1 - 12]) translate([x - 2, -0.01, esp_z0 + 0.1]) cube([4, bump_t + 0.01, esp_pcb_t + 1]);
}

// Snap beam on the VMA432 side: the board pushes it aside on the sloped top and clicks under the lip.
// It touches the board edge at board level and steps back below the board, clear of the Dupont plugs.
module esp_clip() {
    y_in  = esp_y1;            // touches the board edge (the bumps on the other wall take the clearance)
    y_low = esp_y1 + clip_set;
    z_in  = esp_z0 + 0.3;      // inner face starts just above the PCB bottom, clear of the header strip
    z_lip = esp_top + 0.2;
    lip_h = 0.4;
    yz_extrude(esp_x0 + clip_x0, clip_len) polygon([
        [y_low + clip_t, 0], [y_low, 0], [y_low, z_in - clip_set],       // lower part, set back
        [y_in, z_in], [y_in, z_lip],                                     // 45 deg step in, then the face
        [y_in - clip_lip, z_lip], [y_in - clip_lip, z_lip + lip_h],      // lip
        [y_in, z_lip + lip_h + clip_lip], [y_low + clip_t, z_lip + lip_h + clip_lip]  // sloped top
    ]);
}

// The VMA432 goes in nose first at an angle (XLR nose and PCB front into wall B), then the rear is
// lowered between the side guides. The rear swings down in front of the backstops, so they can sit close.
module vma_supports() {
    for (s = [-1, 1]) {
        // rests at the rear corners, clear of the header solder joints
        translate([vma_x0 + 3, vma_cy + s * (vma_w / 2 - 3), 0]) cylinder(d = 5, h = vma_standoff);
        // rests just behind wall B, under the PCB edges
        translate([flange_x - 3, vma_cy + s * (vma_w / 2 - 1.5) - 1.5, 0]) cube([3 + 0.01, 3, vma_standoff]);
        // side guides along the rear, running back into the backstops
        gy = s > 0 ? vma_cy + vma_w / 2 + clr : vma_cy - vma_w / 2 - clr - guide_t;
        translate([vma_x0 - bs_gap - 2, gy, 0]) cube([bs_gap + 2 + 9, guide_t, vma_top + 1.5]);
        // backstops behind the rear PCB corners, clear of the Dupont plugs. Even without the lid bracket
        // a push can move the module at most bs_gap, less than wire_bend: the plugs never reach wall A.
        by0 = s > 0 ? vma_cy + vma_w / 2 - 4 : gy;
        translate([vma_x0 - bs_gap - 2, by0, 0]) cube([2, 4 + clr + guide_t, vma_top + 1.5]);
    }
}
bs_gap = 1.5;   // backstops behind the PCB rear edge (was 3.5 on the first print; the user asked for 2 mm closer)
// Upside down the ESP32 drops onto the lid posts; it must never reach the BOOT nub (the stop button)
assert(holddown_gap < nub_gap, "nub_gap must be larger than holddown_gap, or an upside-down box could hold BOOT down");
assert(esp_x0 + clip_x0 + clip_len < vma_x0 - bs_gap - 2 || esp_y1 + clip_set + clip_t < vma_cy - vma_w / 2 - clr - guide_t,
       "ESP32 clip runs into the VMA432 guides");
assert(bs_gap < wire_bend, "VMA432 backstops would let the Dupont plugs reach wall A: raise wire_bend");

module base(with_ears = mount_ears) {
    difference() {
        union() {
            difference() {
                union() {
                    translate([0, 0, -floor_t]) chamfered_prism(floor_t + H_in, bed_ch) outline_box();
                    for (p = tower_xy) translate([p[0], p[1], -floor_t]) chamfered_cylinder(tower_r, floor_t + H_in, bed_ch);
                    if (with_ears) for (side = [0, 1]) translate([0, 0, -floor_t]) chamfered_prism(ear_t, 0.6) ear_2d(side);
                }
                cube([L_in, W_in, H_in + 1]);
            }
            xlr_pad();
        }
        usb_cutout(usb_cz, H_in);
        yz_extrude(flange_x - 1, xlr_nose + 2) xlr_opening_2d();
        vents();
        for (p = tower_xy) translate([p[0], p[1], H_in - pilot_depth]) cylinder(d = pilot_d, h = pilot_depth + 1);
        if (with_ears) for (side = [0, 1]) ear_cuts(side);
    }
    esp_supports();
    vma_supports();
}

// ---------------------------------------------------------------- lid
module lid_lip() {
    difference() {
        translate([0, 0, H_in - lip_h]) linear_extrude(lip_h + 0.01) difference() {
            translate([clr, clr]) square([L_in - 2 * clr, W_in - 2 * clr]);
            translate([clr + lip_t, clr + lip_t]) square([L_in - 2 * (clr + lip_t), W_in - 2 * (clr + lip_t)]);
        }
        translate([-1, usb_y - usb_shell_w / 2 - 1, H_in - lip_h - 1]) cube([clr + lip_t + 2, usb_shell_w + 2, lip_h + 2]);
        translate([L_in - clr - lip_t - 1, xlr_y - latch_slot_w / 2 - 1, H_in - lip_h - 1])
            cube([clr + lip_t + 2, latch_slot_w + 2, lip_h + 2]);
    }
}

// Label text, reading left to right with the USB end toward you.
module label(txt, depth) {
    linear_extrude(depth + 0.01) rotate(-90)
        text(txt, size = label_size, font = label_font, halign = "center", valign = "center");
}

function tab_x0(t) = btn_xy(t[0])[0] - nub_d / 2 - 0.3;  // free end, toward wall A
function tab_yc(t) = btn_xy(t[0])[1] + t[3];

module btn_tab_cuts() {
    for (t = btn_tabs) {
        x0 = tab_x0(t);
        y = tab_yc(t);
        w = t[2];
        // U-shaped cut through the lid and its lip, open on the hinge side
        translate([0, 0, H_in - lip_h - 1]) linear_extrude(lip_h + lid_t + 2) difference() {
            translate([x0 - tab_slot, y - w / 2 - tab_slot]) square([tab_len + tab_slot, w + 2 * tab_slot]);
            translate([x0, y - w / 2]) square([tab_len + 1, w]);
        }
        // thin the tab from below
        translate([x0, y - w / 2, H_in - lip_h - 1]) cube([tab_len, w, lip_h + 1 + lid_t - tab_t]);
        translate([x0 + tab_len / 2, y, H_in + lid_t - label_depth]) label(t[1], label_depth);
    }
}

// Engraved lid text, centred between the button tabs and wall B. It reads like the labels
// (USB end toward you), so the first line is the one furthest from the USB end.
module lid_text_cut() {
    n = len(lid_text);
    heights = [for (i = [0 : n - 1]) lid_text_size[i]];
    total = sum(heights) + (n - 1) * lid_text_gap;
    x_lo = max([for (t = btn_tabs) tab_x0(t) + tab_len]) + 2;
    xc = (x_lo + L_in) / 2;
    for (i = [0 : n - 1]) {
        above = i == 0 ? 0 : sum([for (j = [0 : i - 1]) lid_text_size[j]]) + i * lid_text_gap;
        x = xc + total / 2 - above - lid_text_size[i] / 2;
        translate([x, W_in / 2, H_in + lid_t - lid_text_depth]) linear_extrude(lid_text_depth + 0.01) rotate(-90)
            text(lid_text[i], size = lid_text_size[i], font = label_font, halign = "center", valign = "center");
    }
}
function sum(v, i = 0) = i >= len(v) ? 0 : v[i] + sum(v, i + 1);

module btn_nubs() {
    z0 = esp_top + esp_btn_h + nub_gap;
    for (t = btn_tabs) {
        p = btn_xy(t[0]);
        translate([p[0], p[1], z0]) cylinder(d = nub_d, h = H_in + lid_t - tab_t - z0 + 0.01);
    }
}

module lid_holddowns() {
    // on the ESP32 module can
    z0 = esp_top + esp_shield_h + holddown_gap;
    if (z0 < H_in)
        for (dx = [-5, 5]) translate([esp_x1 - esp_shield_cx + dx, esp_cy, z0]) cylinder(d = holddown_d, h = H_in - z0 + 0.01);
    // bracket behind and on top of the XLR housing: takes the push when a plug goes in, keeps the module down
    bx = xlr_rear_x - clr - 2;
    translate([bx, xlr_y - 6, xlr_top_z - 6]) cube([2, 12, H_in - (xlr_top_z - 6) + 0.01]);
    translate([bx, xlr_y - 6, xlr_top_z + holddown_gap]) cube([2 + clr + 6, 12, H_in - xlr_top_z - holddown_gap + 0.01]);
    // tongue that closes the USB-C slot above the port and keeps the port down
    zu = usb_shell_top + clr;
    zr = usb_cz + usb_recess_h / 2;
    if (zu < H_in)
        translate([-wall + usb_recess, usb_y - usb_shell_w / 2, zu]) cube([wall - usb_recess, usb_shell_w, H_in - zu + 0.01]);
    if (zr < H_in)
        translate([-wall, usb_y - usb_shell_w / 2, zr]) cube([usb_recess + 0.01, usb_shell_w, H_in - zr + 0.01]);
    // fill the latch slot above the latch, if there is room
    zt = latch_top + 1;
    if (zt < H_in - 0.5)
        translate([L_in, xlr_y - latch_slot_w / 2 + clr, zt]) cube([wall, latch_slot_w - 2 * clr, H_in - zt + 0.01]);
}

module lid() {
    difference() {
        union() {
            translate([0, 0, H_in]) chamfered_prism(lid_t, lid_ch, true) outline_box();
            // screw ears: full thickness, no chamfer, merged solidly into the lid corner
            for (p = tower_xy) hull() {
                translate([p[0], p[1], H_in]) cylinder(r = tower_r, h = lid_t);
                translate([p[0] < 0 ? -wall + 2 : L_in + wall - 2, p[1] < 0 ? -wall + 2 : W_in + wall - 2, H_in])
                    cylinder(r = 2, h = lid_t);
            }
            lid_lip();
            lid_holddowns();
        }
        for (p = tower_xy) translate([p[0], p[1], H_in - 1]) {
            cylinder(d = screw_d, h = lid_t + 2);
            if (screw_head == "countersunk")
                translate([0, 0, 1 + lid_t - (cs_d - screw_d) / 2]) cylinder(d1 = screw_d, d2 = cs_d, h = (cs_d - screw_d) / 2 + 0.01);
        }
        btn_tab_cuts();
        lid_text_cut();
    }
    btn_nubs();  // after the cuts, which would otherwise trim them
}

// ---------------------------------------------------------------- test coupon
// 1. ESP32 frame: the board drops in with its pins through the window and rests on two end ledges
//    (PCB bottom at z = 0). The end wall has the real USB opening at the real height above the PCB.
// 2. Button gauge: lay it on the board against the end wall; the holes must be over BOOT and EN.
// 3. XLR corner: a slice of the real base. Put the VMA432 on the posts and plug your cable in.
cp_t    = 2.0;
frame_t = 2.0;

module esp_coupon() {
    gz = 1.5;
    y_hi = esp_y1 + clr;
    usb_c = esp_pcb_t + esp_usb_z;
    wall_top = usb_c + usb_recess_h / 2 + 1;
    difference() {
        union() {
            for (y = [-frame_t, y_hi]) translate([-wall, y, -cp_t]) cube([esp_x1 + clr + frame_t + wall, frame_t, cp_t + gz]);
            // end ledges only between the header rows, which run almost to the board ends
            translate([0, esp_cy - 8.5, -cp_t]) cube([esp_x0 + 3, 17, cp_t]);
            translate([esp_x1 - 3, esp_cy - 8.5, -cp_t]) cube([3 + clr, 17, cp_t]);
            translate([esp_x1 + clr, 0, -cp_t]) cube([frame_t, y_hi, cp_t + gz]);
            translate([-wall, -frame_t, -cp_t]) cube([wall, y_hi + 2 * frame_t, cp_t + wall_top]);
        }
        usb_cutout(usb_c, wall_top);
    }
}

module button_gauge() {
    // modelled in use orientation: underside at z = 0, tabs hang down outside the frame rails
    g_l = max(esp_btn_boot[0], esp_btn_en[0]) + esp_x0 + label_size + 8;
    y_lo = -frame_t - clr - 1.2;
    y_hi = esp_y1 + clr + frame_t + clr + 1.2;
    difference() {
        union() {
            translate([0, y_lo, 0]) cube([g_l, y_hi - y_lo, 1.2]);
            for (y = [y_lo, y_hi - 1.2]) translate([0, y, -4]) cube([g_l, 1.2, 4]);
        }
        // holes where the lid nubs will land; the board's buttons must show through them
        for (t = btn_tabs) {
            p = btn_xy(t[0]);
            translate([p[0], p[1], -1]) cylinder(d = gauge_hole_d, h = 3);
            translate([p[0] + gauge_hole_d / 2 + 1 + label_size / 2, p[1], 1.2 - 0.5]) label(t[1], 0.5);
        }
    }
}

module xlr_corner() {
    x0 = vma_x0 - 3;
    y0 = max(vma_cy - vma_w / 2 - 6, esp_y1 + clr + guide_t + 0.2);  // leave the ESP32 guides out
    intersection() {
        base(false);
        translate([x0, y0, -floor_t - 1])
            cube([L_in + tower_off + tower_r + 1 - x0, W_in + tower_off + tower_r + 1 - y0, H_in + floor_t + 2]);
    }
}

module coupon() {
    translate([0, 0, cp_t]) esp_coupon();
    // printed face down; after the flip it spans y = [-gauge_y_hi, ...], so shift it clear of the frame
    gauge_y_hi = esp_y1 + clr + frame_t + clr + 1.2;
    translate([0, esp_y1 + clr + frame_t + 6 + gauge_y_hi, 1.2]) rotate([180, 0, 0]) button_gauge();
    translate([esp_x1 + 12 - (vma_x0 - 3), -(vma_cy - vma_w / 2), floor_t]) xlr_corner();
}

// ---------------------------------------------------------------- ghosts (fit check only)
module esp_ghost() {
    color("seagreen") translate([esp_x0, esp_y0, esp_z0]) cube([esp_l, esp_w, esp_pcb_t]);
    color("silver") translate([esp_x1 - esp_shield_cx - 9, esp_cy - 9, esp_top]) cube([18, 18, esp_shield_h]);
    color("darkslategray") translate([esp_x1 - esp_shield_cx + 9, esp_cy - 9, esp_top]) cube([esp_shield_cx - 9, 18, 0.8]);
    color("lightgray") translate([esp_x0 - esp_usb_overhang, usb_y - usb_shell_w / 2, usb_cz - usb_shell_h / 2])
        cube([7.35, usb_shell_w, usb_shell_h]);
    for (b = [esp_btn_boot, esp_btn_en]) {
        p = btn_xy(b);
        color("ivory") translate([p[0] - 1.5, p[1] - 2, esp_top]) cube([3, 4, esp_btn_h]);
    }
    row_len = esp_row_pins * 2.54;
    rx = esp_x0 + (esp_l - row_len) / 2;
    for (s = [-1, 1]) {
        color("black") translate([rx, esp_cy + s * esp_row_pitch / 2 - 1.27, esp_z0 - 2.5]) cube([row_len, 2.54, 2.5]);
        color("gold") translate([rx, esp_cy + s * esp_row_pitch / 2 - 0.32, esp_z0 - 8.5]) cube([row_len, 0.64, 6]);
    }
    // Dupont housings on the 38-pin board, counted from the USB end: 5V and GND (VMA side), GPIO17 (other row)
    for (d = [[1, 0], [1, 5], [-1, 8]])
        color("dimgray") translate([rx + d[1] * 2.54, esp_cy + d[0] * esp_row_pitch / 2 - 1.27, esp_z0 - 16.5]) cube([2.54, 2.54, 14]);
}

module vma_ghost() {
    color("white") translate([vma_x0, vma_cy - vma_w / 2, vma_standoff]) cube([vma_l, vma_w, vma_pcb_t]);
    color("#222222") {
        translate([xlr_rear_x, xlr_y - xlr_left + 1, vma_top]) cube([xlr_body - 1.5, xlr_left + xlr_right - 2, xlr_cz + xlr_up]);
        difference() {
            translate([flange_x - 1.5, xlr_y - xlr_left, vma_top]) cube([1.5, xlr_left + xlr_right, xlr_cz + xlr_up]);
            for (s = [-1, 1]) translate([flange_x - 2, xlr_y + s * xlr_ear, xlr_z - s * xlr_ear]) rotate([0, 90, 0]) cylinder(d = 2.35, h = 3);
        }
        translate([flange_x, xlr_y, xlr_z]) rotate([0, 90, 0]) cylinder(d = xlr_nose_d, h = xlr_nose);
    }
    // metal PUSH latch, sticks out in front of the nose
    color("silver") translate([face_x - 0.5, xlr_y - xlr_latch_w / 2, xlr_z + xlr_nose_d / 2 - 3])
        cube([xlr_latch_out + 0.5, xlr_latch_w, latch_top - (xlr_z + xlr_nose_d / 2 - 3)]);
    color("black") translate([vma_x0 + 0.5, vma_cy - 5.08, vma_top]) cube([2.5, 10.16, 2.5]);
    for (i = [0, 1, 3]) color("dimgray") translate([vma_x0 - 14, vma_cy - 5.08 + i * 2.54, vma_top]) cube([14, 2.54, 2.54]);
}

module ghosts() { esp_ghost(); vma_ghost(); }

// ---------------------------------------------------------------- output
module cut() {
    if (cut_y >= 0) intersection() { children(); translate([-50, cut_y, -50]) cube([500, 500, 500]); }
    else children();
}

if (part == "assembly") {
    cut() {
        color("#3b4a5a") base();
        color("#8fa3b8", 0.45) translate([0, 0, explode]) lid();
    }
    if (show_assembly) ghosts();
} else if (part == "base") {
    translate([0, 0, floor_t]) {
        base();
        if (show_assembly) %ghosts();
    }
} else if (part == "lid") {
    translate([0, 0, H_in + lid_t]) rotate([180, 0, 0]) lid();
} else if (part == "coupon") {
    coupon();
}
