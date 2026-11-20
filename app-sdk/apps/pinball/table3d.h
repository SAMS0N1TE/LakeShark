/* The renderer uses the collision table's units: x right, v down-table,
   height above the playfield. No separate decorative collision layout. */
static int m_steel,m_dark,m_wood,m_felt[2],m_rubber[2],m_lamp[2],m_white,m_gold,m_off,m_cap,m_screen;
static int m_print[2],m_insert[2],m_apron[2],m_card;

static l3_vec table_at(float u,float v,float h)
{ return l3v((u-50)*.1f,h*.1f,(v-91)*.12f); }

static void table_box(float u,float v,float h,float w,float d,float tall,int mat)
{
    l3_xform t=l3_identity();
    l3_vec p=table_at(u,v,h);
    t.m[9]=p.x; t.m[10]=p.y; t.m[11]=p.z;
    l3_box(scene,&t,l3v(w*.05f,tall*.05f,d*.06f),mat);
}

static void tube(float ax,float ay,float bx,float by,float h,float radius,int mat)
{ l3_cylinder(scene,table_at(ax,ay,h),table_at(bx,by,h),radius*.1f,mat); }

static void setup_3d(void)
{
    static const uint8_t black[]={0,8};
    static const uint8_t reef[]={0,2,2,6};
    static const uint8_t ion[]={4,5};
    static const uint8_t cap[]={7,15};
    static const uint8_t red[]={1,9,11};
    static const uint8_t blue[]={4,6,14};
    static const uint8_t amber[]={3,11};
    static const uint8_t cyan[]={6,14};
    static const uint8_t screen[]={0};
    m_steel=l3_material(scene,L3_RAMP_METAL,4,.95f,0);
    m_dark=l3_material(scene,black,2,.15f,0);
    m_wood=l3_material(scene,L3_RAMP_WOOD,4,.25f,L3_GRAIN);
    m_felt[REEF]=l3_material(scene,reef,4,.12f,0);
    /* Broad, flat tonal steps keep the printed playfield quiet. Curved
       parts carry the shading detail, rather than dither over every dot. */
    m_felt[ION]=l3_material(scene,ion,2,0,L3_FLAT);
    m_cap=l3_material(scene,cap,2,.7f,L3_FLAT);
    m_rubber[REEF]=l3_material(scene,red,3,.15f,0);
    m_rubber[ION]=l3_material(scene,blue,3,.3f,L3_FLAT);
    m_lamp[REEF]=l3_material(scene,amber,2,0,L3_UNLIT|L3_GLOW);
    m_lamp[ION]=l3_material(scene,cyan,2,0,L3_UNLIT|L3_GLOW);
    m_white=l3_material(scene,L3_RAMP_IVORY,4,.55f,0);
    m_gold=l3_material(scene,L3_RAMP_WOOD,4,.8f,0);
    m_off=l3_material(scene,black,2,.6f,0);
    m_screen=l3_material(scene,screen,1,0,L3_UNLIT);
    /* Printed art and unlit inserts: flat, so the field reads as artwork and
       not as dither; an insert off is a dull plastic, on it is the lamp. */
    static const uint8_t print_reef[]={4,4},print_ion[]={4,6};   /* REEF: deep water blue, the felt's own green vanished */
    static const uint8_t insert_reef[]={3},insert_ion[]={4};
    static const uint8_t apron_reef[]={3,11},apron_ion[]={8,7},card[]={7,15};
    m_print[REEF]=l3_material(scene,print_reef,2,0,L3_FLAT);
    m_print[ION]=l3_material(scene,print_ion,2,0,L3_FLAT);
    m_insert[REEF]=l3_material(scene,insert_reef,1,0,L3_UNLIT);
    m_insert[ION]=l3_material(scene,insert_ion,1,0,L3_UNLIT);
    m_apron[REEF]=l3_material(scene,apron_reef,2,.2f,L3_FLAT);
    m_apron[ION]=l3_material(scene,apron_ion,2,.2f,L3_FLAT);
    m_card=l3_material(scene,card,2,0,L3_FLAT);
    l3_sun(scene,l3v(-.6f,-1,-.45f),.85f,.28f);
    l3_shadows(scene,true);
    l3_sky(scene,LAP_BLACK,LAP_BLACK);
    l3_outline(scene,LAP_DOT_CLEAR);
}

static void set_view(void)
{
    if(!view_dirty) return;
    /* Both views keep the entire playfield visible; the second is steeper
       for aiming. The camera stays still during play for the static cache. */
    l3_vec eye=camera_mode?l3v(0,25,8):l3v(0,24,13);
    /* Aim at the complete machine's centre, not the playfield origin.
       Otherwise the tall backbox forces a zoom-out with unused space below. */
    l3_vec target=l3v(0,4.7f,-.05f);
    float distance=.65f;
    /* Fit the cabinet, including the shooter lane, on narrow boards as
       well as landscape screens. Only evaluated when the view changes. */
    for(int step=0;step<65;step++) {
        l3_camera(scene,l3_add(target,l3_scale(eye,distance)),target,44);
        bool fits=true;
        for(int i=0;i<4;i++) {
            float x,y;
            l3_vec p=table_at(i&1?102:-2,i&2?186:-5,i&2?0:94);
            if(!l3_project(scene,p,&x,&y) || x<2 || x>cv.w-2 || y<hud_h+1 || y>cv.h-3) fits=false;
        }
        if(fits) break;
        distance*=1.02f;
    }
    l3_clip(scene,1,150);
    view_dirty=false;
}

static void draw_flipper3d(const flipper_t *f)
{
    float dx=f->side*cosf(f->ang)*FL_LEN,dy=sinf(f->ang)*FL_LEN;
    l3_vec a=table_at(f->px,f->py,2.2f),b=table_at(f->px+dx,f->py+dy,2.2f);
    l3_cone(scene,a,b,.32f,.16f,m_rubber[machine]);
    a.y+=.13f; b.y+=.13f;
    l3_cone(scene,a,b,.23f,.10f,m_white);
    l3_cylinder(scene,table_at(f->px,f->py,3),table_at(f->px,f->py,4),.11f,m_steel);
}

/* A dot-matrix score glass built into the backbox. Its placement follows
   the projected cabinet in both camera views, rather than a screen HUD. */
static void draw_backbox(uint32_t now)
{
    float lx,ty,rx,by;
    if(!l3_project(scene,table_at(7,5,89),&lx,&ty) ||
       !l3_project(scene,table_at(93,5,8),&rx,&by)) return;
    int x=(int)lx+1,y=(int)ty+1,w=(int)(rx-lx)-2,h=(int)(by-ty)-2;
    if(w<28 || h<9) return;
    uint8_t ink=machine==REEF?LC_BR_YELLOW:LC_BR_CYAN,dim=machine==REEF?LC_YELLOW:LC_CYAN;
    lc_fill_rect(&cv,x,y,w,h,LC_BLACK);
    lc_rect(&cv,x,y,w,h,dim);
    /* backglass art round the score: REEF a sun rising in the top corners,
       ION a bolt down each side */
    if(h>=23) {
        for(int side=0;side<2;side++) {
            int cx=side?x+w-2:x+1,dir=side?-1:1;
            if(machine==REEF) {
                lc_fill_circle(&cv,cx,y+1,3,LC_YELLOW);
                for(int r=0;r<4;r++) lc_line(&cv,cx,y+1,cx+dir*(6+r*3),y+3+r*2+(r&1),LC_YELLOW);
            } else {
                int bx=cx+dir*2;
                lc_line(&cv,bx,y+2,bx+dir*3,y+h/3,LC_CYAN);
                lc_line(&cv,bx+dir*3,y+h/3,bx,y+h/3+1,LC_CYAN);
                lc_line(&cv,bx,y+h/3+1,bx+dir*2,y+h*2/3,LC_BR_CYAN);
            }
        }
    }
    char t[32];
    if(state==ST_TITLE) snprintf(t,sizeof(t),"BEST %d",bests[level]);
    else snprintf(t,sizeof(t),"%d",score);
    int k=lc_text_w(t,2)<w-4 && h>=23 ? 2:1;
    lc_text(&cv,x+(w-lc_text_w(t,k))/2,y+2,t,k,ink);
    if(h>=23) {
        snprintf(t,sizeof(t),"BALL %d/%d  %dX",ball_no,BALLS,mult);
        lc_text(&cv,x+(w-lc_text_w(t,1))/2,y+5+5*k,t,1,LC_WHITE);
    }
    /* A clipped marquee: title, instructions and awards scroll across the
       same physical display while the score stays in its own row. */
    if(h<16) return;
    const char *message=tilted?"TILT - FLIPPERS DISABLED":now<msg_until?msg:
        state==ST_TITLE?MACHINE_NAME[machine]:in_lane?"HOLD RIGHT - RELEASE TO SHOOT":
        state==ST_OVER?"GAME OVER - TAP TO CONTINUE":
        machine==ION?"ION STORM - FIVE SPINS FOR ION SURGE":"REEF CLUB - CLEAR DROPS FOR JACKPOT";
    /* its own inset strip, a rule above it, a cell of margin each side: it ran
       into the frame */
    const int sy=y+h-9;
    lc_line(&cv,x+2,sy-1,x+w-3,sy-1,dim);
    int n=(int)strlen(message),cells=30;
    char window[32];
    int offset=0;
    for(;;) {                       /* as many characters as really fit, measured */
        offset=n>cells?(int)(now/180)%(n+4):0;
        for(int i=0;i<cells;i++) {int at=(offset+i)%(n+4);window[i]=at<n?message[at]:' ';}
        window[cells]=0;
        if(lc_text_w(window,1)<=w-8 || cells<=1) break;
        cells--;
    }
    if(n<=cells) {window[n]=0;}     /* short: shown whole, centred */
    lc_text(&cv,x+(w-lc_text_w(window,1))/2,sy+1,window,1,ink);
    if(charging) lc_fill_rect(&cv,x+2,y+h-2,(int)((w-4)*power),1,ink);
}

/* Flat things printed on, or set into, the playfield, just above the felt.
   Quads are one-sided: these are drawn L3_TWO_SIDED.

   An arrowhead insert pointing along `deg` (0 = right, 90 = down the table),
   notched at the back: four distinct corners (a triangle given as a quad with
   a repeated corner was not drawn). */
static void insert_arrow(float u,float v,float deg,float size,int mat)
{
    float c=cosf(deg*PI/180),s=sinf(deg*PI/180),w=size*.65f;
    float tu=u+c*size,tv=v+s*size,bu=u-c*size*.8f,bv=v-s*size*.8f,nu=u-c*size*.3f,nv=v-s*size*.3f;
    l3_quad(scene,table_at(tu,tv,.12f),table_at(bu-s*w,bv+c*w,.12f),table_at(nu,nv,.12f),table_at(bu+s*w,bv-c*w,.12f),mat);
}

/* A printed stripe from a to b, `w` wide. */
static void stripe(float au,float av,float bu,float bv,float w,float h,int mat)
{
    float du=bu-au,dv=bv-av,l=sqrtf(du*du+dv*dv);
    if(l<.01f) return;
    float nu=-dv/l*w*.5f,nv=du/l*w*.5f;
    l3_quad(scene,table_at(au+nu,av+nv,h),table_at(bu+nu,bv+nv,h),
        table_at(bu-nu,bv-nv,h),table_at(au-nu,av-nv,h),mat);
}

/* The printed playfield: REEF a two-tone sunburst behind the pops and a swell
   across the lower field; ION a lightning bolt between its four pops and
   circuit traces from each pop to the spinner. Static, so it costs one pass. */
static void playfield_art(uint32_t now)
{
    l3_flags(scene,L3_STATIC|L3_TWO_SIDED);
    if(machine==REEF) {
        for(int i=0;i<16;i+=2) {            /* alternate wedges of the burst */
            float a0=i*PI/8,a1=(i+1)*PI/8;
            l3_quad(scene,table_at(46+13*cosf(a0),91+13*sinf(a0),.06f),table_at(46+29*cosf(a0),91+29*sinf(a0),.06f),
                table_at(46+29*cosf(a1),91+29*sinf(a1),.06f),table_at(46+13*cosf(a1),91+13*sinf(a1),.06f),m_print[REEF]);
        }
        for(int i=0;i<16;i++) {             /* gold rules between them */
            float a=i*PI/8;
            l3_line(scene,table_at(46+11*cosf(a),91+11*sinf(a),.1f),table_at(46+31*cosf(a),91+31*sinf(a),.1f),LC_YELLOW);
        }
        l3_disc(scene,table_at(46,91,.2f),l3v(0,1,0),1.15f,m_gold);
        l3_disc(scene,table_at(46,91,.25f),l3v(0,1,0),.95f,m_felt[REEF]);
        for(int w=0;w<2;w++)                /* the swell: two rolling wave lines */
            for(int k=0;k<14;k++) {
                float u0=22+k*3.6f,u1=u0+3.6f,v=124+w*6;
                l3_line(scene,table_at(u0,v+2*sinf(k*.9f+w),.1f),table_at(u1,v+2*sinf((k+1)*.9f+w),.1f),w?LC_WHITE:LC_BR_CYAN);
            }
    } else {
        static const float bolt[][2]={{50,30},{40,60},{53,63},{43,96}};
        const bool surge=fabsf(spinner_speed)>3;
        l3_flags(scene,L3_TWO_SIDED);                  /* the bolt flares with the spinner: not cached */
        for(int i=0;i<3;i++) stripe(bolt[i][0],bolt[i][1],bolt[i+1][0],bolt[i+1][1],3.2f,.06f,surge?m_lamp[ION]:m_print[ION]);
        l3_flags(scene,L3_STATIC|L3_TWO_SIDED);
        for(int i=0;i<bumper_count;i++) {   /* traces from the lower pops to the spinner, a pad at each bend */
            if(bumpers[i].y<70) continue;   /* the upper ones would cross the bolt */
            float bu=bumpers[i].x,bv=bumpers[i].y,ku=bu<46?bu+6:bu-6;
            l3_line(scene,table_at(bu,bv+7,.1f),table_at(bu,bv+12,.1f),LC_CYAN);
            l3_line(scene,table_at(bu,bv+12,.1f),table_at(ku,bv+18,.1f),LC_CYAN);
            l3_line(scene,table_at(ku,bv+18,.1f),table_at(bu<46?40:52,105,.1f),LC_CYAN);
            l3_point(scene,table_at(ku,bv+18,.15f),1.5f,LC_BR_CYAN);
        }
        l3_flags(scene,L3_STATIC|L3_TWO_SIDED);
        l3_line(scene,table_at(36,142,.1f),table_at(46,132,.1f),LC_CYAN);
        l3_line(scene,table_at(46,132,.1f),table_at(56,142,.1f),LC_CYAN);
    }
    (void)now;
}

/* Inserts that say what to shoot: arrows at the drop bank and the right
   targets while any are still up (blinking), the multiplier ladder, the
   rollover lamps in chrome rims, and SHOOT AGAIN on the apron. */
static void inserts(uint32_t now)
{
    const int lamp=m_lamp[machine],off=m_insert[machine];
    const bool blink=(now/260)%2;
    l3_flags(scene,L3_TWO_SIDED);
    /* still to hit: blinking; done: dark (a drop is down) or solid (a target is lit) */
    for(int i=0;i<3;i++) insert_arrow(18,DROP_V[i],180,4.4f,drop_up[i]&&blink?lamp:off);
    for(int i=0;i<4;i++) insert_arrow(75,MESH_V[i],0,4.2f,mesh_lit[i]||blink?lamp:off);
    for(int i=0;i<4;i++) insert_arrow(34+i*8,137,-90,3.9f,mult>=i+2?lamp:off);
    for(int i=0;i<3;i++) {
        l3_disc(scene,table_at(LANE_X[i],36,.2f),l3v(0,1,0),.5f,m_steel);
        l3_disc(scene,table_at(LANE_X[i],36,.25f),l3v(0,1,0),.38f,lane_lit[i]?lamp:off);
        insert_arrow(LANE_X[i],44,-90,3.0f,lane_lit[i]?lamp:off);
    }
}

/* The apron: a flat plate with a chrome lip, an instruction card either side
   (ruled lines for print) and the SHOOT AGAIN lamp between them. */
static void apron(uint32_t now)
{
    l3_flags(scene,L3_STATIC|L3_CAST);
    table_box(50,173,.5f,78,14,1,m_apron[machine]);
    tube(11,166,89,166,1.2f,.35f,m_steel);
    l3_flags(scene,L3_STATIC|L3_TWO_SIDED);
    for(int side=0;side<2;side++) {
        float u0=side?62:16,u1=u0+22;
        l3_quad(scene,table_at(u0,169,1.15f),table_at(u1,169,1.15f),table_at(u1,178,1.15f),table_at(u0,178,1.15f),m_card);
        for(int r=0;r<3;r++)
            l3_line(scene,table_at(u0+2,171+r*2.4f,1.2f),table_at(u1-2-(r==2?6:0),171+r*2.4f,1.2f),LC_BLACK);
    }
    l3_flags(scene,L3_TWO_SIDED);
    const bool saving=now<save_until;
    l3_disc(scene,table_at(50,173,1.2f),l3v(0,1,0),.62f,m_steel);
    l3_disc(scene,table_at(50,173,1.25f),l3v(0,1,0),.5f,saving&&(now/200)%2?m_lamp[machine]:m_insert[machine]);
}

static void draw_table(uint32_t now)
{
    set_view();
    const int lamp=m_lamp[machine],rubber=m_rubber[machine];
    l3_begin(scene);
    l3_flags(scene,L3_STATIC|L3_CAST);
    /* A real cabinet: wooden/painted walls below the glass, an apron and
       a raised backbox with chrome edging. */
    table_box(50,91,-7.5f,104,188,14,machine==REEF?m_wood:m_dark);
    table_box(50,91,-.2f,94,178,.4f,m_felt[machine]);
    table_box(2,91,2,4,186,7,m_steel);
    table_box(98,91,2,4,186,7,m_steel);
    table_box(50,181,2,100,4,7,m_steel);
    table_box(50,0,48,102,7,92,machine==REEF?m_wood:m_dark);
    table_box(50,4,49,88,1,82,m_screen);
    tube(6,5,94,5,90,.65f,m_steel);
    tube(6,5,94,5,7,.65f,m_steel);
    playfield_art(now);
    apron(now);
    if(machine==ION) {
        tube(3.5f,55,3.5f,168,4,.32f,lamp);
        tube(96.5f,55,96.5f,168,4,.32f,lamp);
    }
    /* Every wall and target comes from build_table's physics segments. */
    for(int i=0;i<nseg;i++) {
        const seg_t *s=&segs[i];
        l3_flags(scene,L3_STATIC|L3_CAST);
        if(s->kind==K_WALL || s->kind==K_GATE) {
            tube(s->ax,s->ay,s->bx,s->by,2,.65f,m_steel);
        } else if(s->kind==K_RUBBER || s->kind==K_KICK) {
            tube(s->ax,s->ay,s->bx,s->by,2,1,s->kind==K_KICK?rubber:m_white);
            l3_cylinder(scene,table_at(s->ax,s->ay,0),table_at(s->ax,s->ay,3),.13f,m_steel);
        } else if(s->kind==K_DROP) {
            l3_flags(scene,L3_CAST);
            float h=drop_up[s->idx]?3.3f:.3f;
            table_box(6,(s->ay+s->by)*.5f,h,3,7,drop_up[s->idx]?6: .5f,rubber);
            if(drop_up[s->idx]) table_box(7.6f,(s->ay+s->by)*.5f,4, .3f,3,1.5f,m_white);
        } else if(s->kind==K_MESH) {
            l3_flags(scene,L3_CAST);
            table_box(86.5f,(s->ay+s->by)*.5f,2.5f,3,7,5,mesh_lit[s->idx]?lamp:rubber);
        }
    }
    /* Slingshot plastics sit above their rubber triangles. */
    for(int side=0;side<2;side++) {
        float a=side?84:8,b=side?73:19;
        bool hot=sling_lit[side] && now-sling_lit[side]<130;
        l3_flags(scene,L3_CAST);
        l3_quad(scene,table_at(a,116,3.4f),table_at(a,139,3.4f),
            table_at(b,147,3.4f),table_at(a,116,3.4f),hot?lamp:m_white);
    }
    for(int i=0;i<bumper_count;i++) {
        const bumper_t *b=&bumpers[i];
        bool hot=b->lit && now-b->lit<130;
        l3_flags(scene,L3_CAST);
        l3_cylinder(scene,table_at(b->x,b->y,0),table_at(b->x,b->y,1.2f),b->r*.115f,m_dark);
        l3_cone(scene,table_at(b->x,b->y,1.2f),table_at(b->x,b->y,3),b->r*.10f,b->r*.075f,hot?lamp:rubber);
        if(machine==ION) {
            /* One readable crown over a broad illuminated skirt. Avoid
               several sub-pixel rings fighting within a theme cell. */
            l3_cylinder(scene,table_at(b->x,b->y,3),table_at(b->x,b->y,4.5f),b->r*.085f,lamp);
            l3_xform crown=l3_pose(table_at(b->x,b->y,5),l3_quat_axis(l3v(0,1,0),0),1);
            l3_ellipsoid(scene,&crown,l3v(b->r*.08f,.17f,b->r*.08f),hot?lamp:m_cap);
        } else {
            l3_cylinder(scene,table_at(b->x,b->y,3),table_at(b->x,b->y,5),b->r*.082f,m_steel);
            l3_disc(scene,table_at(b->x,b->y,5.1f),l3v(0,1,0),b->r*.06f,hot?lamp:m_white);
            l3_disc(scene,table_at(b->x,b->y,5.2f),l3v(0,1,0),.13f,lamp);
        }
    }
    inserts(now);
    if(machine==ION) {
        l3_flags(scene,L3_STATIC|L3_CAST);
        for(int i=0;i<2;i++) tube(39+14*i,104,39+14*i,106,5,.6f,m_steel);
        tube(39,105,53,105,6,.45f,m_steel);
        l3_flags(scene,L3_CAST);
        l3_xform t=l3_pose(table_at(46,105,6),l3_quat_axis(l3v(1,0,0),spinner_angle*57.29578f),1);
        l3_box(scene,&t,l3v(.48f,.30f,.035f),fabsf(spinner_speed)>3?lamp:rubber);
    }
    l3_flags(scene,L3_CAST);
    draw_flipper3d(&flip[0]); draw_flipper3d(&flip[1]);
    /* Plunger rod and compression coils. */
    float pull=in_lane?power*5:0;
    tube(92,176+pull,92,185,2,.6f,m_steel);
    for(int i=0;i<4;i++) tube(90,177+pull+i*(1.7f-pull*.15f),94,177+pull+i*(1.7f-pull*.15f),2,.35f,m_steel);
    table_box(92,176+pull,2,5,1.4f,2,m_white);
    const v2 bp=in_lane?(v2){LANE_U,LANE_V+pull}:ball;
    if(state!=ST_DRAIN && state!=ST_OVER) {
        l3_sphere(scene,table_at(bp.x,bp.y,BALL_R),BALL_R*.1f,m_steel);
        l3_point(scene,table_at(bp.x-.8f,bp.y-.8f,BALL_R*1.9f),1.5f,LC_BR_WHITE);   /* the glint: the ball reads at a glance */
    }
    PB_MARK(2);
    l3_end(scene);
    PB_MARK(3);
    memcpy(cv.d,l3_dots(scene),(size_t)cv.w*cv.h);
    draw_backbox(now);
    {   /* the machine's name on the field, in the board's own font (as a 3D label
           it was too small to read: ION came out as TON) */
        float x,y;
        const char *name=machine==REEF?"REEF":"ION";
        if(l3_project(scene,machine==REEF?table_at(46,91,.3f):table_at(46,125,.3f),&x,&y))
            lc_text_outlined(&cv,(int)x-lc_text_w(name,1)/2,(int)y-3,name,1,machine==REEF?LC_BR_YELLOW:LC_BR_CYAN,LC_BLACK);
    }
    /* Floating scores use the camera projection, so they stay on the hit. */
    for(int i=0;i<POPS;i++) {
        if(!pops[i].t0) continue;
        uint32_t age=now-pops[i].t0;
        if(age>700) { pops[i].t0=0; continue; }
        float x,y;
        if(l3_project(scene,table_at(pops[i].u,pops[i].v,6+age*.014f),&x,&y))
            lc_text_outlined(&cv,(int)x-lc_text_w(pops[i].s,1)/2,(int)y,pops[i].s,1,LC_BR_WHITE,LC_BLACK);
    }
}
