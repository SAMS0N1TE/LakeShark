// LakeShark landscape handheld, dimensions in millimetres.
// front, rear, lcd_clip, divider and section select individual exports.
part = "assembly";
$fn = 48;
eps = 0.04;

// Measured inputs. Battery values describe the casing only, excluding its plug.
pack_width = 112;
pack_height = 65;
pack_depth = 8.8;
pack_xy_clearance = 1.5;    // Per side
pack_top_clearance = 1.8;
display_stack = 16.0;      // Glass face to lowest populated connector with harness installed
glass_thickness = 3.0;     // Verify the exposed glass/frame edge before fitting clips
display_footprint = [114.4, 66.8];
display_active = [94.4, 56.96];
active_offset = [0, 0];    // Relative to glass centre, in the front view
tolerance = 0.45;

centre_half_h = max(43, pack_height/2 + 13.5);
grip_deck = -0.85;
wall = 2.6;
front_skin = 3.7;
floor_skin = 2.6;
bezel_step = 1.05;
seam = -5.8;
divider_thickness = 1.6;
divider_top = -(display_stack + 1.5);
divider_bottom = divider_top - divider_thickness;
rear_bottom = divider_bottom - pack_top_clearance - pack_depth - floor_skin;
divider_bolt_x = pack_width/2 + pack_xy_clearance + 3;
divider_bolts = [for (side=[-1,1], y=[-14,4]) [side*divider_bolt_x,y]];

// The −90° left pod puts the stick high. The +90° right pod puts it low.
controller_x = 84.5;
controller_y = -4.0;
controller_origins = [[-controller_x,controller_y], [controller_x,controller_y]];
controller_angles = [-90,90];
pcb_size = [62.4, 30.4];
pcb_top = -7.2;
pcb_mounts = [[-28,-12],[-28,12],[28,-12],[28,12]];
stick_centre = [-17.9, 0.8];
stick_opening = 20.6;
// Locations are calculated from the TK21 dimensioned front view.
diamond = [[19.8,6.1],[25.2,0.2],[19.3,-5.9],[13.6,0.2]];
utility = [[-1.0,-7.1],[5.3,-7.1]];
centre_bolt_y = centre_half_h - 7.5;
rear_bolts = [[-90,-40.2],[-controller_x,31.6],
              [90,-40.2],[controller_x,31.6],
              [-48,-centre_bolt_y],[-48,centre_bolt_y],
              [48,-centre_bolt_y],[48,centre_bolt_y]];
clip_x = 61.1;
clip_y = 20.0;
clip_z = -bezel_step - glass_thickness - 0.8;
clip_t = 3.2;

assert(pack_width > 0 && pack_height > 0 && pack_depth > 0, "Enter pack dimensions in mm");
assert(pack_width + 2*pack_xy_clearance <= 122, "This cradle needs a narrower pack or a wider electronics layout");
assert(pack_height + 2*pack_xy_clearance <= 2*centre_half_h - 18, "Pack obstructs the service area");
assert(display_stack > 9 && glass_thickness > 0, "Measure the display and attached harness stack");

module rect(size, radius) {
    offset(r=radius) square([size.x - 2*radius, size.y - 2*radius], center=true);
}

module centre_body() {
    polygon([[-60,centre_half_h],[60,centre_half_h],[69,centre_half_h-9],
        [69,-centre_half_h+10],[61,-centre_half_h+1],[-61,-centre_half_h+1],
        [-69,-centre_half_h+10],[-69,centre_half_h-9]]);
}

module right_body() {
    polygon([[62,28],[73,38],[91,40],[102,31],[104,23],[104,-28],
        [100,-42],[90,-49],[77,-48],[63,-39],[60,-28],[60,20]]);
}

module pod_shape(side) {
    if (side < 0) mirror([1,0,0]) right_body();
    else right_body();
}

module body() {
    union() {
        centre_body();
        for (side=[-1,1]) pod_shape(side);
    }
}

module controller(index) {
    translate(controller_origins[index]) rotate(controller_angles[index]) children();
}

module prism(z_low, z_high) {
    translate([0,0,z_low]) linear_extrude(height=z_high-z_low) children();
}

module round_prism(size, radius, z_low, z_high) {
    prism(z_low,z_high) rect(size,radius);
}

module drill(point, diameter, z_low, z_high) {
    translate([point.x,point.y,z_low]) cylinder(h=z_high-z_low,d=diameter);
}

module rise(z_low,z_high,upper_inset) {
    hull() {
        prism(z_low,z_low+eps) children();
        prism(z_high-eps,z_high) offset(delta=-upper_inset,chamfer=true) children();
    }
}

module centre_panel() {
    polygon([[-57.5,centre_half_h-2],[57.5,centre_half_h-2],
        [66,centre_half_h-9.5],[66,-centre_half_h+12],[58.5,-centre_half_h+3],
        [-58.5,-centre_half_h+3],[-66,-centre_half_h+12],[-66,centre_half_h-9.5]]);
}

module right_panel() {
    polygon([[66.5,28],[76,33],[91,34],[100,28],[102,21],[102,-27],
        [98,-40],[89.5,-46],[77.5,-45],[66.5,-36]]);
}

module pod_panel(side) {
    if (side < 0) mirror([1,0,0]) right_panel();
    else right_panel();
}

module control_holes() {
    translate(stick_centre) circle(d=stick_opening);
    // The stick frame rises 5.3 mm above the PCB and intersects the
    // underside of the skin. Clear its rectangular shoulders as well.
    translate([-18,-0.1]) rect([19.2,18.2],0.8);
    hull()
        for (point=diamond) translate(point) circle(d=8.8);
    hull()
        for (point=utility) translate(point) rotate(45) rect([9.0,6.4],1.4);
}

module bezel() {
    difference() {
        union() {
            prism(-front_skin,-1.95) body();
            rise(-2.0,0,1.15) centre_panel();
            for (side=[-1,1]) rise(-2.0,grip_deck,1.0) pod_panel(side);
            prism(seam,-front_skin+eps)
                difference() { body(); offset(delta=-wall) body(); }
            round_prism(display_footprint + [4.8,4.8],5.0,-5.0,-front_skin+eps);
            for (index=[0:1]) controller(index)
                for (point=pcb_mounts)
                    drill(point,5.7,pcb_top,-front_skin+eps);
            for (point=rear_bolts) drill(point,7.4,seam+0.20,-front_skin+eps);
            for (side=[-1,1], y=[-clip_y,clip_y])
                drill([side*clip_x,y],6.0,clip_z+eps,-front_skin+eps);
        }
        translate(active_offset)
            round_prism(display_active + [1.4,1.4],1.0,-6,eps);
        round_prism(display_footprint + [2*tolerance,2*tolerance],4.0,-6,-bezel_step);
        for (index=[0:1]) controller(index) {
            prism(-front_skin-0.2,eps) control_holes();
            // Covered clearance for the right-angle 2 mm header and socket.
            translate([0,18.8])
                round_prism([24,16],2.0,pcb_top-eps,-2.45);
        }
        for (index=[0:1]) controller(index)
            for (point=pcb_mounts)
                drill(point,2.25,pcb_top-eps,-2.15);
        for (point=rear_bolts) drill(point,2.65,seam-2,-2.2);
        for (side=[-1,1], y=[-clip_y,clip_y])
            drill([side*clip_x,y],2.25,min(-8,clip_z-eps),-0.35);
    }
}

module divider_outline() {
    rect([2*(divider_bolt_x+2.75), pack_height + 13],3.0);
}

module divider() {
    difference() {
        prism(divider_bottom,divider_top)
            difference() {
                divider_outline();
                translate([-pack_width/2-3,pack_height/2+5])
                    rect([10,8],2);
            }
        for (point=divider_bolts) drill(point,2.8,divider_bottom-eps,divider_top+eps);
        for (point=rear_bolts) drill(point,8.2,divider_bottom-eps,divider_top+eps);
    }
}

module battery_ribs() {
    bx=pack_width/2+pack_xy_clearance;
    by=pack_height/2+pack_xy_clearance;
    for (side=[-1,1]) {
        translate([side*(bx+1.1),-13])
            round_prism([2.2,25],1.0,rear_bottom+floor_skin-eps,rear_bottom+floor_skin+5);
        translate([20,side*(by+1.1)])
            round_prism([32,2.2],1.0,rear_bottom+floor_skin-eps,rear_bottom+floor_skin+5);
        translate([side*(bx+3),-6])
            round_prism([6.0,32],1.2,rear_bottom+floor_skin-eps,divider_bottom);
    }
}

module rear_exterior() {
    prism(rear_bottom+3.5-eps,seam) body();
    mirror([0,0,1]) {
        rise(-rear_bottom-3.5,-rear_bottom,3.0) centre_body();
        for (side=[-1,1])
            rise(-rear_bottom-3.5,-rear_bottom,3.0) pod_shape(side);
    }
}

module tray() {
    difference() {
        union() {
            difference() {
                rear_exterior();
                prism(rear_bottom+floor_skin,seam+eps) offset(delta=-wall) body();
            }
            intersection() {
                rear_exterior();
                union() for (point=rear_bolts) drill(point,7.3,rear_bottom,seam);
            }
            battery_ribs();
        }
        for (point=rear_bolts) {
            drill(point,3.25,rear_bottom-eps,seam+eps*2);
            drill(point,6.3,rear_bottom-eps,rear_bottom+1.5);
        }
        for (point=divider_bolts)
            drill(point,2.25,max(rear_bottom+floor_skin,divider_bottom-5),divider_bottom+eps);
        // Wide service opening for the SD card, RESET, BOOT and POWER.
        translate([9,centre_half_h])
            round_prism([82,11],2.0,divider_top+0.8,seam+eps*2);
        // A rear-facing route for USB-C pigtails on either display orientation.
        for (side=[-1,1])
            translate([side*80,0])
                round_prism([27,29],4.0,rear_bottom-eps,rear_bottom+floor_skin+eps);
        // Lower vents sit outboard of the battery compartment.
        for (side=[-1,1], slot=[0:3])
            translate([side*(72+9*slot),-31])
                round_prism([4,9],1.8,rear_bottom-eps,rear_bottom+floor_skin+eps);
    }
}

module clip() {
    difference() {
        union() {
            round_prism([5.7,2*clip_y+6],2.25,clip_z-clip_t,clip_z);
            for (y=[-clip_y+5,clip_y-5])
                translate([-2.8,y]) round_prism([7.6,9.0],2.2,clip_z-clip_t,clip_z);
        }
        for (y=[-clip_y,clip_y]) drill([0,y],2.8,clip_z-clip_t-eps,clip_z+eps);
    }
}

module hardware() {
    color("#202a35") round_prism(display_footprint,4.0,-bezel_step-glass_thickness,-bezel_step);
    color("#1c8290") translate(active_offset)
        round_prism(display_active-[0.4,0.4],0.6,-bezel_step,-bezel_step+0.06);
    for (index=[0:1]) controller(index) {
        color("#1b2526") round_prism(pcb_size,3.0,pcb_top-1.6,pcb_top);
        color("#26292d") drill(stick_centre,14.8,pcb_top+9.5,pcb_top+13.55);
        for (point=diamond) color("#343941") drill(point,7.4,pcb_top,pcb_top+4.3);
        for (point=utility) color("#ca6247")
            translate(point) rotate(45) round_prism([4.7,2.7],0.6,pcb_top,pcb_top+3.0);
    }
    color("#acb1ba") round_prism([pack_width,pack_height],3.4,
        rear_bottom+floor_skin,rear_bottom+floor_skin+pack_depth);
}

module assembled() {
    color("#303940") bezel();
    color("#24282e") tray();
    color("#aaa9a3") divider();
    color("#39474d") translate([clip_x,0,0]) clip();
    color("#39474d") translate([-clip_x,0,0]) mirror([1,0,0]) clip();
    hardware();
}

if (part=="front") rotate([180,0,0]) bezel();
else if (part=="rear") translate([0,0,-rear_bottom]) tray();
else if (part=="lcd_clip") translate([0,0,-clip_z+clip_t]) clip();
else if (part=="divider") translate([0,0,-divider_bottom]) divider();
else if (part=="section") difference() {
    assembled();
    translate([0,-100,-100]) cube([200,200,200]);
}
else assembled();
