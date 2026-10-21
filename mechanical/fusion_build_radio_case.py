"""Table-top wedge enclosure for the ESP32-P4 internet radio (Guition JC1060P470C_I_W, 7").

Run inside Fusion (Scripts and Add-Ins, or the Fusion MCP 'script' tool) in an empty design.

Everything is modelled in the FACE frame (mm):
    u = X  across the face, 0 = left outer edge (seen from the front)
    v = Y  up along the face, 0 = bottom outer edge
    w = Z  out of the face towards the user, 0 = front of the body / back of the panel
and at the end each component is rotated -FACE_TILT about X, so the face leans back
FACE_TILT degrees (90 - 15 = 75 deg to the table) and the wedge base sits flat.

Parts (separate bodies; Fusion "Part Design" documents allow one component):
    Body        hollow wedge, open at the front: USB-C slot (left), microSD slot + finger notch
                (top), 4 posts with pins that press the PCB at its mounting holes, 4 screw bosses
    FrontPanel  3 mm plate: screen window, speaker grille + holding ring, 4 countersunk M3 holes
    Speaker_REF 40 x 5 mm speaker      (reference only, do not print)
    Display_REF module glass + PCB      (reference only, do not print)

Board data: Guition JC1060P470C_I_W structure drawing (guitionofficial/P4-series,
3-Structure_Diagram/02.png). Values marked CHECK were read off that drawing / product photo
and should be confirmed with calipers before printing.
"""
import adsk.core, adsk.fusion, math

# ---- board (front view, landscape) -------------------------------------------------------
PCB_W, PCB_H = 165.0, 104.0        # PCB outline (touch glass is 165.0 x 100.3)
STACK = 7.0                        # glass front -> PCB back (TP 1.6 + LCD 3.5 + PCB 1.6 + glue)
HOLE_DX, HOLE_DY = 156.0, 94.0     # 4 x D2.1 mounting holes, centred on the PCB
AA_W, AA_H = 154.21, 85.92         # active area
AA_LEFT, AA_BOTTOM = 4.3, 10.1     # active area offset from the PCB's left / bottom edge  (CHECK)
SD_U = 61.5                        # microSD socket centre from the PCB's left edge        (CHECK)
SD_W = 15.4                        # socket width
USB_V0, USB_V1 = 28.0, 84.0        # USB-C ports span on the left edge, from the PCB bottom (CHECK)

# ---- enclosure -------------------------------------------------------------------------
FACE_TILT = 15.0                   # face leans back 15 deg -> 75 deg to the table
WALL = 2.5
CLR = 0.5                          # clearance around the PCB
PANEL_T = 3.0
MB = 10.0                          # bottom margin below the PCB (chin)
W = 220.0                          # overall width (fits a 256/270 mm bed)
BASE_D = 85.0                      # base depth on the table (world, horizontal)
TOP_D = 35.0                       # depth of the horizontal top (world)
WIN_MARGIN = 1.2                   # screen window larger than the active area, each side

SPK_D, SPK_T = 40.0, 5.0           # speaker
SPK_RING_ID, SPK_RING_WALL = 40.8, 2.0
GRILLE_D, GRILLE_HOLE, GRILLE_PITCH = 36.0, 2.4, 3.6

POST_D, PIN_D, PIN_L = 6.0, 1.8, 1.2
BOSS_D, BOSS_PILOT, BOSS_DEPTH = 8.0, 2.5, 12.0     # M3 self-tapping, 12 mm deep
SCREW_D, CSK_D = 3.4, 6.4

# ---- derived ---------------------------------------------------------------------------
PCB_U0 = WALL + CLR                # PCB left edge
PCB_V0 = MB                        # PCB bottom edge
PCB_U1, PCB_V1 = PCB_U0 + PCB_W, PCB_V0 + PCB_H
H = PCB_V1 + CLR + WALL            # face height
SPK_U = (PCB_U1 + W - WALL) / 2 + 1.0
SPK_V = (PCB_V0 + PCB_V1) / 2      # mid side bar
W_PCB_BACK = -STACK
HOLES = [(PCB_U0 + (PCB_W - HOLE_DX) / 2 + i * HOLE_DX, PCB_V0 + (PCB_H - HOLE_DY) / 2 + j * HOLE_DY)
         for i in (0, 1) for j in (0, 1)]
BOSSES = [(6.5, 5.0), (PCB_U0 + PCB_W / 2, 5.0), (W - 6.5, 5.0), (W - 6.5, H - 6.5)]

T = math.radians(FACE_TILT)


def world_to_face(y, z):
    """World side-profile point (y up, z towards the user) -> face frame (v, w)."""
    return (y * math.cos(T) - z * math.sin(T), y * math.sin(T) + z * math.cos(T))


# wedge side profile (world): front-bottom edge at the origin, horizontal top, flat base
_top_y, _top_z = H * math.cos(T), -H * math.sin(T)
PROFILE = [world_to_face(0, 0), world_to_face(_top_y, _top_z),
           world_to_face(_top_y, _top_z - TOP_D), world_to_face(0, -BASE_D)]


def cm(v):
    return v / 10.0


app = adsk.core.Application.get()
design = adsk.fusion.Design.cast(app.activeProduct)
root = design.rootComponent
NEW = adsk.fusion.FeatureOperations.NewBodyFeatureOperation
JOIN = adsk.fusion.FeatureOperations.JoinFeatureOperation
CUT = adsk.fusion.FeatureOperations.CutFeatureOperation
INTERSECT = adsk.fusion.FeatureOperations.IntersectFeatureOperation


class _Part:
    """All parts live as bodies in the root component (Part Design documents allow only one)."""
    def __init__(self, name):
        self.name = name
        self.component = root


def new_comp(name):
    return _Part(name)


def plane_xy(comp, w):
    if abs(w) < 1e-9:
        return comp.xYConstructionPlane
    pi = comp.constructionPlanes.createInput()
    pi.setByOffset(comp.xYConstructionPlane, adsk.core.ValueInput.createByReal(cm(w)))
    p = comp.constructionPlanes.add(pi)
    p.isLightBulbOn = False
    return p


def P(sk, u, v, w):
    return sk.modelToSketchSpace(adsk.core.Point3D.create(cm(u), cm(v), cm(w)))


def profiles(sk):
    oc = adsk.core.ObjectCollection.create()
    for p in sk.profiles:
        oc.add(p)
    return oc


def extrude(comp, profs, dist, op, body=None):
    ei = comp.features.extrudeFeatures.createInput(profs, op)
    ei.setDistanceExtent(False, adsk.core.ValueInput.createByReal(cm(dist)))
    if body is not None and op != NEW:
        ei.participantBodies = [body]
    return comp.features.extrudeFeatures.add(ei)


def box(comp, u0, u1, v0, v1, w0, w1, op, body=None):
    """Axis-aligned box in the face frame (w0 < w1)."""
    sk = comp.sketches.add(plane_xy(comp, w0))
    sk.sketchCurves.sketchLines.addTwoPointRectangle(P(sk, u0, v0, w0), P(sk, u1, v1, w0))
    return extrude(comp, profiles(sk), w1 - w0, op, body)


def cyl(comp, u, v, d, w0, w1, op, body=None):
    """Cylinder along w from w0 to w1 (w0 < w1)."""
    sk = comp.sketches.add(plane_xy(comp, w0))
    sk.sketchCurves.sketchCircles.addByCenterRadius(P(sk, u, v, w0), cm(d / 2))
    return extrude(comp, profiles(sk), w1 - w0, op, body)


def slot(comp, u0, u1, v0, v1, w0, w1, op, body=None):
    """Rounded-end slot (stadium) in the face frame, long axis along whichever side is longer."""
    sk = comp.sketches.add(plane_xy(comp, w0))
    lines, arcs = sk.sketchCurves.sketchLines, sk.sketchCurves.sketchArcs
    du, dv = u1 - u0, v1 - v0
    if dv >= du:
        r = du / 2
        cu = (u0 + u1) / 2
        a, b = v0 + r, v1 - r
        lines.addByTwoPoints(P(sk, u0, a, w0), P(sk, u0, b, w0))
        lines.addByTwoPoints(P(sk, u1, a, w0), P(sk, u1, b, w0))
        arcs.addByThreePoints(P(sk, u0, b, w0), P(sk, cu, v1, w0), P(sk, u1, b, w0))
        arcs.addByThreePoints(P(sk, u0, a, w0), P(sk, cu, v0, w0), P(sk, u1, a, w0))
    else:
        r = dv / 2
        cv = (v0 + v1) / 2
        a, b = u0 + r, u1 - r
        lines.addByTwoPoints(P(sk, a, v0, w0), P(sk, b, v0, w0))
        lines.addByTwoPoints(P(sk, a, v1, w0), P(sk, b, v1, w0))
        arcs.addByThreePoints(P(sk, b, v0, w0), P(sk, u1, cv, w0), P(sk, b, v1, w0))
        arcs.addByThreePoints(P(sk, a, v0, w0), P(sk, u0, cv, w0), P(sk, a, v1, w0))
    return extrude(comp, profiles(sk), w1 - w0, op, body)


def side_box(comp, v0, v1, w0, w1, u0, u1, op, body=None):
    """Box sketched on the side (YZ) plane, extruded along u: for cuts through the left wall."""
    pi = comp.constructionPlanes.createInput()
    pi.setByOffset(comp.yZConstructionPlane, adsk.core.ValueInput.createByReal(cm(u0)))
    pl = comp.constructionPlanes.add(pi)
    pl.isLightBulbOn = False
    sk = comp.sketches.add(pl)
    lines, arcs = sk.sketchCurves.sketchLines, sk.sketchCurves.sketchArcs
    r = (w1 - w0) / 2
    cw = (w0 + w1) / 2
    a, b = v0 + r, v1 - r
    lines.addByTwoPoints(P(sk, u0, a, w0), P(sk, u0, b, w0))
    lines.addByTwoPoints(P(sk, u0, a, w1), P(sk, u0, b, w1))
    arcs.addByThreePoints(P(sk, u0, b, w0), P(sk, u0, v1, cw), P(sk, u0, b, w1))
    arcs.addByThreePoints(P(sk, u0, a, w0), P(sk, u0, v0, cw), P(sk, u0, a, w1))
    return extrude(comp, profiles(sk), u1 - u0, op, body)


def combine(target, tools, op, keep=False):
    comp = target.parentComponent
    oc = adsk.core.ObjectCollection.create()
    for t in tools:
        oc.add(t)
    ci = comp.features.combineFeatures.createInput(target, oc)
    ci.operation = op
    ci.isKeepToolBodies = keep
    return comp.features.combineFeatures.add(ci)


def appearance(body, name):
    lib = app.materialLibraries.itemByName("Fusion Appearance Library")
    if lib:
        a = lib.appearances.itemByName(name)
        if a:
            body.appearance = a


def build_body():
    occ = new_comp("Body")
    comp = occ.component
    # solid wedge: side profile on the YZ plane, extruded across the width
    sk = comp.sketches.add(comp.yZConstructionPlane)
    pts = [P(sk, 0, v, w) for v, w in PROFILE]
    lines = sk.sketchCurves.sketchLines
    for i in range(len(pts)):
        lines.addByTwoPoints(pts[i], pts[(i + 1) % len(pts)])
    wedge = extrude(comp, profiles(sk), W, NEW).bodies.item(0)
    wedge.name = "Body"

    # PCB posts and screw bosses: long cylinders trimmed to the wedge (before shelling)
    far = min(w for _, w in PROFILE) - 5
    extras = []
    for (u, v) in HOLES:
        extras.append(cyl(comp, u, v, POST_D, far, W_PCB_BACK, NEW).bodies.item(0))
    for (u, v) in BOSSES:
        extras.append(cyl(comp, u, v, BOSS_D, far, 0.0, NEW).bodies.item(0))
    for b in extras:
        combine(b, [wedge], INTERSECT, keep=True)

    # shell, open at the front face (the face lying in the w = 0 plane)
    front = None
    for f in wedge.faces:
        g = f.geometry
        if isinstance(g, adsk.core.Plane) and abs(abs(g.normal.z) - 1) < 1e-6 and abs(g.origin.z) < 1e-6:
            front = f
    oc = adsk.core.ObjectCollection.create()
    oc.add(front)
    si = comp.features.shellFeatures.createInput(oc, False)
    si.insideThickness = adsk.core.ValueInput.createByReal(cm(WALL))
    comp.features.shellFeatures.add(si)
    combine(wedge, extras, JOIN)

    # pins into the PCB holes, pilot holes in the bosses
    for (u, v) in HOLES:
        cyl(comp, u, v, PIN_D, W_PCB_BACK, W_PCB_BACK + PIN_L, JOIN, wedge)
    for (u, v) in BOSSES:
        cyl(comp, u, v, BOSS_PILOT, -BOSS_DEPTH, 0.5, CUT, wedge)

    # USB-C: one rounded slot through the left wall (fits the plug overmold, 8 mm tall)
    usb_w = W_PCB_BACK - 1.6
    side_box(comp, PCB_V0 + USB_V0, PCB_V0 + USB_V1, usb_w - 4.0, usb_w + 4.0, -1.0, WALL + CLR + 0.1, CUT, wedge)

    # microSD: slot through the top wall along the card's insertion direction (face-up),
    # plus a finger notch from the front so the card can be pushed / pulled
    sd_u = PCB_U0 + SD_U
    sd_w = W_PCB_BACK - 0.9
    box(comp, sd_u - SD_W / 2 - 0.5, sd_u + SD_W / 2 + 0.5, PCB_V1 - 2, H + 20, sd_w - 2.0, sd_w + 2.0, CUT, wedge)
    slot(comp, sd_u - 10, sd_u + 10, PCB_V1 + CLR, H + 20, -13.0, 0.1, CUT, wedge)
    appearance(wedge, "Plastic - Matte (Black)")
    return occ


def build_panel():
    occ = new_comp("FrontPanel")
    comp = occ.component
    panel = box(comp, 0, W, 0, H, 0, PANEL_T, NEW).bodies.item(0)
    panel.name = "FrontPanel"
    # screen window
    box(comp, PCB_U0 + AA_LEFT - WIN_MARGIN, PCB_U0 + AA_LEFT + AA_W + WIN_MARGIN,
        PCB_V0 + AA_BOTTOM - WIN_MARGIN, PCB_V0 + AA_BOTTOM + AA_H + WIN_MARGIN,
        -1, PANEL_T + 1, CUT, panel)
    # speaker grille (hex pattern inside a circle) + holding ring behind the panel
    sk = comp.sketches.add(plane_xy(comp, -1))
    circles = sk.sketchCurves.sketchCircles
    rows = int(GRILLE_D / GRILLE_PITCH) + 2
    for j in range(-rows, rows + 1):
        dv = j * GRILLE_PITCH * math.sqrt(3) / 2
        off = (GRILLE_PITCH / 2) if j % 2 else 0.0
        for i in range(-rows, rows + 1):
            du = i * GRILLE_PITCH + off
            if math.hypot(du, dv) + GRILLE_HOLE / 2 <= GRILLE_D / 2:
                circles.addByCenterRadius(P(sk, SPK_U + du, SPK_V + dv, -1), cm(GRILLE_HOLE / 2))
    extrude(comp, profiles(sk), PANEL_T + 2, CUT, panel)
    ring_od = SPK_RING_ID + 2 * SPK_RING_WALL
    sk = comp.sketches.add(plane_xy(comp, -(SPK_T + 0.6)))
    circles = sk.sketchCurves.sketchCircles
    circles.addByCenterRadius(P(sk, SPK_U, SPK_V, -(SPK_T + 0.6)), cm(ring_od / 2))
    circles.addByCenterRadius(P(sk, SPK_U, SPK_V, -(SPK_T + 0.6)), cm(SPK_RING_ID / 2))
    ring = [p for p in sk.profiles if p.profileLoops.count == 2]
    oc = adsk.core.ObjectCollection.create()
    for p in ring:
        oc.add(p)
    extrude(comp, oc, SPK_T + 0.6, JOIN, panel)
    # countersunk M3 holes into the body's bosses
    for (u, v) in BOSSES:
        cyl(comp, u, v, SCREW_D, -1, PANEL_T + 1, CUT, panel)
        sk = comp.sketches.add(plane_xy(comp, PANEL_T))
        sk.sketchCurves.sketchCircles.addByCenterRadius(P(sk, u, v, PANEL_T), cm(CSK_D / 2))
        ei = comp.features.extrudeFeatures.createInput(profiles(sk), CUT)
        ei.setOneSideExtent(adsk.fusion.DistanceExtentDefinition.create(
            adsk.core.ValueInput.createByReal(cm((CSK_D - SCREW_D) / 2))),
            adsk.fusion.ExtentDirections.NegativeExtentDirection,
            adsk.core.ValueInput.createByString("-45 deg"))
        ei.participantBodies = [panel]
        comp.features.extrudeFeatures.add(ei)
    appearance(panel, "Plastic - Matte (Black)")
    return occ


def build_refs():
    occ = new_comp("Speaker_REF")
    comp = occ.component
    b = cyl(comp, SPK_U, SPK_V, SPK_D, -SPK_T, 0, NEW).bodies.item(0)
    b.name = "Speaker_REF"
    appearance(b, "Steel - Satin")
    occ2 = new_comp("Display_REF")
    comp = occ2.component
    g = box(comp, PCB_U0, PCB_U1, PCB_V0 + 1.85, PCB_V1 - 1.85, -3.4, 0, NEW).bodies.item(0)
    g.name = "Glass"
    appearance(g, "Glass (Window)") if False else appearance(g, "Plastic - Glossy (Black)")
    p = box(comp, PCB_U0, PCB_U1, PCB_V0, PCB_V1, W_PCB_BACK, W_PCB_BACK + 1.6, NEW).bodies.item(0)
    p.name = "PCB"
    appearance(p, "Paint - Enamel Glossy (Yellow)")
    return [occ, occ2]


def tilt(_parts):
    """Lean the face back FACE_TILT degrees: the wedge base lands flat on the XZ plane."""
    m = adsk.core.Matrix3D.create()
    m.setToRotation(math.radians(-FACE_TILT), adsk.core.Vector3D.create(1, 0, 0),
                    adsk.core.Point3D.create(0, 0, 0))
    oc = adsk.core.ObjectCollection.create()
    for b in root.bRepBodies:
        oc.add(b)
    mi = root.features.moveFeatures.createInput2(oc)
    mi.defineAsFreeMove(m)
    root.features.moveFeatures.add(mi)


def run(_context: str):
    design.designType = adsk.fusion.DesignTypes.ParametricDesignType
    body = build_body()
    panel = build_panel()
    refs = build_refs()
    tilt([body, panel] + refs)
    print("Built: W=%.1f mm, face height H=%.1f mm, speaker at (%.1f, %.1f), base %.0f x %.0f mm" %
          (W, H, SPK_U, SPK_V, W, BASE_D))
