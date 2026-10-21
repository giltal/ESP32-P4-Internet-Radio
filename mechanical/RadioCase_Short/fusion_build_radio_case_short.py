"""SHORT table-top wedge enclosure, speaker on the back (Guition JC1060P470C_I_W, 7").

Variant of ../fusion_build_radio_case.py: no speaker bar beside the screen (the case is only as
wide as the board needs); a 20 mm speaker sits in the sloped back wall behind a grille.

Run inside Fusion (Scripts and Add-Ins, or the Fusion MCP 'script' tool). Each run builds the
case in a NEW design document.

Everything is modelled in the FACE frame (mm):
    u = X  across the face, 0 = left outer edge (seen from the front)
    v = Y  up along the face, 0 = bottom outer edge
    w = Z  out of the face towards the user, 0 = front of the body / back of the panel
and at the end all bodies are rotated -FACE_TILT about X, so the face leans back FACE_TILT
degrees (75 deg to the table) and the wedge base sits flat on the table.

Parts (separate bodies; Fusion "Part Design" documents allow one component):
    Body        rounded hollow wedge, open at the front. The board is screwed to it: 4 posts end
                on the board's brass M2 standoffs (inner holes, 90 x 52 mm); an access tunnel
                through the back of each post takes an M2 x 8 screw and a screwdriver.
                USB-C slot (left), microSD slot + finger notch (top), 4 bosses for the panel.
                Rear speaker: grille through the back wall + holding ring inside (20 mm speaker).
    FrontPanel  3 mm plate cut from the body's rounded front outline: screen window,
                5 countersunk M3 holes.
    Speaker_REF, Glass, PCB, Standoff_REF   reference only, do not print

Assembly: board in from the front -> 4 x M2 x 8 from the back into the brass standoffs ->
speaker into the back-wall ring (dab of hot glue) -> panel on with 5 x M3 x 10 countersunk.

Board data: Guition JC1060P470C_I_W structure drawing (guitionofficial/P4-series,
3-Structure_Diagram/02.png) and product photo. Values marked CHECK should be measured.
"""
import adsk.core, adsk.fusion, math

# ---- board (front view, landscape) -------------------------------------------------------
PCB_W, PCB_H = 165.0, 104.0        # PCB outline (touch glass is 165.0 x 100.3)
STACK = 7.0                        # glass front -> PCB back (TP 1.6 + LCD 3.5 + PCB 1.6 + glue)
MOUNT_DX, MOUNT_DY = 90.0, 52.0    # inner M2 holes with brass standoffs, centred on the PCB
STANDOFF_H = 3.0                   # brass standoff height above the PCB back (measured)
STANDOFF_D = 3.5                   # (reference body only)
AA_W, AA_H = 154.21, 85.92         # active area
AA_LEFT, AA_BOTTOM = 4.3, 10.1     # active area offset from the PCB's left / bottom edge  (CHECK)
SD_U = 61.5                        # microSD socket centre from the PCB's left edge        (CHECK)
SD_W = 15.4                        # socket width
USB_V0, USB_V1 = 28.0, 84.0        # USB-C ports span on the left edge, from the PCB bottom (CHECK)
COMPONENTS = 10.0                  # tallest parts behind the PCB (pin header, capacitors)

# ---- enclosure -------------------------------------------------------------------------
FACE_TILT = 15.0                   # face leans back 15 deg -> 75 deg to the table
WALL = 2.5
CLR = 0.5                          # clearance around the PCB
PANEL_T = 3.0
MB = 10.0                          # bottom margin below the PCB (chin)
TOP_M = 9.0                        # top margin above the PCB (room for the top panel screws)
BASE_D = 58.0                      # base depth on the table (world, horizontal)
TOP_D = 24.0                       # depth of the horizontal top (world)
ROUND = 6.0                        # outer edge radius of the body
PANEL_ROUND = 1.5                  # front edge radius of the panel
WIN_MARGIN = 1.2                   # screen window larger than the active area, each side
FRONT_GAP = 0.3                    # glass -> panel gap (the screws hold the board, not the panel)

SPK_D, SPK_T = 20.0, 5.0           # rear speaker                               (thickness CHECK)
SPK_RING_ID, SPK_RING_WALL = 20.6, 1.6
GRILLE_D, GRILLE_HOLE, GRILLE_PITCH = 17.0, 2.0, 3.0

POST_D = 7.0                       # post head that the standoff seats on
TUBE_D = 9.0                       # post body around the screw tunnel
TUNNEL_D = 5.5                     # screwdriver + M2 head (3.8 mm) access from the back
POST_SEAT = 6.0                    # solid post length in front of the tunnel (screw grip length)
M2_CLEAR = 2.4
BOSS_D, BOSS_PILOT, BOSS_DEPTH = 7.0, 2.5, 12.0     # M3 self-tapping, 12 mm deep
SCREW_D, CSK_D = 3.4, 6.4

# ---- derived ---------------------------------------------------------------------------
PCB_U0 = WALL + CLR
PCB_V0 = MB
PCB_U1, PCB_V1 = PCB_U0 + PCB_W, PCB_V0 + PCB_H
PCB_UC, PCB_VC = (PCB_U0 + PCB_U1) / 2, (PCB_V0 + PCB_V1) / 2
W = PCB_U1 + CLR + WALL            # overall width: just the board + clearance + walls
H = PCB_V1 + TOP_M                 # face height
SPK_U = W / 2                      # rear speaker: centred across the back
W_GLASS = -FRONT_GAP               # glass front
W_PCB_BACK = W_GLASS - STACK
W_POST_TIP = W_PCB_BACK - STANDOFF_H
W_SEAT = W_POST_TIP - POST_SEAT    # screw head seat, end of the access tunnel
MOUNTS = [(PCB_UC + sx * MOUNT_DX / 2, PCB_VC + sy * MOUNT_DY / 2) for sx in (-1, 1) for sy in (-1, 1)]
BOSSES = [(7.0, 5.0), (PCB_UC, 5.0), (W - 7.0, 5.0), (7.0, H - 5.0), (W - 7.0, H - 5.0)]
SCREW_LEN = round(POST_SEAT + STANDOFF_H - 1.0)   # M2 length that stops short of the PCB

T = math.radians(FACE_TILT)


def world_to_face(y, z):
    """World side-profile point (y up, z towards the user) -> face frame (v, w)."""
    return (y * math.cos(T) - z * math.sin(T), y * math.sin(T) + z * math.cos(T))


_top_y, _top_z = H * math.cos(T), -H * math.sin(T)
PROFILE = [world_to_face(0, 0), world_to_face(_top_y, _top_z),
           world_to_face(_top_y, _top_z - TOP_D), world_to_face(0, -BASE_D)]

app = adsk.core.Application.get()
design = root = None
NEW = adsk.fusion.FeatureOperations.NewBodyFeatureOperation
JOIN = adsk.fusion.FeatureOperations.JoinFeatureOperation
CUT = adsk.fusion.FeatureOperations.CutFeatureOperation
INTERSECT = adsk.fusion.FeatureOperations.IntersectFeatureOperation


def cm(v):
    return v / 10.0


def plane_xy(w):
    if abs(w) < 1e-9:
        return root.xYConstructionPlane
    pi = root.constructionPlanes.createInput()
    pi.setByOffset(root.xYConstructionPlane, adsk.core.ValueInput.createByReal(cm(w)))
    p = root.constructionPlanes.add(pi)
    p.isLightBulbOn = False
    return p


def P(sk, u, v, w):
    return sk.modelToSketchSpace(adsk.core.Point3D.create(cm(u), cm(v), cm(w)))


def coll(items):
    oc = adsk.core.ObjectCollection.create()
    for i in items:
        oc.add(i)
    return oc


def extrude(profs, dist, op, body=None):
    ei = root.features.extrudeFeatures.createInput(profs, op)
    ei.setDistanceExtent(False, adsk.core.ValueInput.createByReal(cm(dist)))
    if body is not None and op != NEW:
        ei.participantBodies = [body]
    return root.features.extrudeFeatures.add(ei)


def box(u0, u1, v0, v1, w0, w1, op, body=None):
    sk = root.sketches.add(plane_xy(w0))
    sk.sketchCurves.sketchLines.addTwoPointRectangle(P(sk, u0, v0, w0), P(sk, u1, v1, w0))
    return extrude(coll(sk.profiles), w1 - w0, op, body)


def cyl(u, v, d, w0, w1, op, body=None):
    """Cylinder along w from w0 to w1 (w0 < w1)."""
    sk = root.sketches.add(plane_xy(w0))
    sk.sketchCurves.sketchCircles.addByCenterRadius(P(sk, u, v, w0), cm(d / 2))
    return extrude(coll(sk.profiles), w1 - w0, op, body)


def slot_xy(u0, u1, v0, v1, w0, w1, op, body=None):
    """Stadium slot in the face plane, long axis along v."""
    sk = root.sketches.add(plane_xy(w0))
    lines, arcs = sk.sketchCurves.sketchLines, sk.sketchCurves.sketchArcs
    r, cu = (u1 - u0) / 2, (u0 + u1) / 2
    a, b = v0 + r, v1 - r
    lines.addByTwoPoints(P(sk, u0, a, w0), P(sk, u0, b, w0))
    lines.addByTwoPoints(P(sk, u1, a, w0), P(sk, u1, b, w0))
    arcs.addByThreePoints(P(sk, u0, b, w0), P(sk, cu, v1, w0), P(sk, u1, b, w0))
    arcs.addByThreePoints(P(sk, u0, a, w0), P(sk, cu, v0, w0), P(sk, u1, a, w0))
    return extrude(coll(sk.profiles), w1 - w0, op, body)


def slot_side(v0, v1, w0, w1, u0, u1, op, body=None):
    """Stadium slot sketched on a YZ plane at u0, extruded along u (through the left wall)."""
    pi = root.constructionPlanes.createInput()
    pi.setByOffset(root.yZConstructionPlane, adsk.core.ValueInput.createByReal(cm(u0)))
    pl = root.constructionPlanes.add(pi)
    pl.isLightBulbOn = False
    sk = root.sketches.add(pl)
    lines, arcs = sk.sketchCurves.sketchLines, sk.sketchCurves.sketchArcs
    r, cw = (w1 - w0) / 2, (w0 + w1) / 2
    a, b = v0 + r, v1 - r
    lines.addByTwoPoints(P(sk, u0, a, w0), P(sk, u0, b, w0))
    lines.addByTwoPoints(P(sk, u0, a, w1), P(sk, u0, b, w1))
    arcs.addByThreePoints(P(sk, u0, b, w0), P(sk, u0, v1, cw), P(sk, u0, b, w1))
    arcs.addByThreePoints(P(sk, u0, a, w0), P(sk, u0, v0, cw), P(sk, u0, a, w1))
    return extrude(coll(sk.profiles), u1 - u0, op, body)


def combine(target, tools, op, keep=False):
    ci = root.features.combineFeatures.createInput(target, coll(tools))
    ci.operation = op
    ci.isKeepToolBodies = keep
    return root.features.combineFeatures.add(ci)


def fillet(edges, r):
    fi = root.features.filletFeatures.createInput()
    try:
        fi.edgeSetInputs.addConstantRadiusEdgeSet(coll(edges), adsk.core.ValueInput.createByReal(cm(r)), True)
    except AttributeError:
        fi.addConstantRadiusEdgeSet(coll(edges), adsk.core.ValueInput.createByReal(cm(r)), True)
    return root.features.filletFeatures.add(fi)


def planar_face_at_z(body, z):
    for f in body.faces:
        g = f.geometry
        if isinstance(g, adsk.core.Plane) and abs(abs(g.normal.z) - 1) < 1e-6 and abs(g.origin.z - cm(z)) < 1e-6:
            return f
    return None


def appearance(body, name):
    lib = app.materialLibraries.itemByName("Fusion Appearance Library")
    a = lib.appearances.itemByName(name) if lib else None
    if a:
        body.appearance = a


def build_body_and_panel():
    # solid wedge: side profile on the YZ plane, extruded across the width
    sk = root.sketches.add(root.yZConstructionPlane)
    pts = [P(sk, 0, v, w) for v, w in PROFILE]
    for i in range(len(pts)):
        sk.sketchCurves.sketchLines.addByTwoPoints(pts[i], pts[(i + 1) % len(pts)])
    wedge = extrude(coll(sk.profiles), W, NEW).bodies.item(0)
    wedge.name = "Body"

    # round every outer edge except the ones on the front face
    front = planar_face_at_z(wedge, 0)
    front_edges = set(e.tempId for e in front.edges)
    fillet([e for e in wedge.edges if e.tempId not in front_edges], ROUND)

    # front panel = the (now rounded) front outline, PANEL_T thick
    front = planar_face_at_z(wedge, 0)
    panel = extrude(coll([front]), PANEL_T, NEW).bodies.item(0)
    panel.name = "FrontPanel"

    # posts (screw tunnel tube + seat) and panel bosses, trimmed to the wedge before shelling
    far = min(w for _, w in PROFILE) - 5
    extras = []
    for (u, v) in MOUNTS:
        tube = cyl(u, v, TUBE_D, far, W_SEAT, NEW).bodies.item(0)
        cyl(u, v, POST_D, W_SEAT, W_POST_TIP, JOIN, tube)
        extras.append(tube)
    for (u, v) in BOSSES:
        extras.append(cyl(u, v, BOSS_D, far, 0.0, NEW).bodies.item(0))
    for b in extras:
        combine(b, [wedge], INTERSECT, keep=True)

    si = root.features.shellFeatures.createInput(coll([planar_face_at_z(wedge, 0)]), False)
    si.insideThickness = adsk.core.ValueInput.createByReal(cm(WALL))
    root.features.shellFeatures.add(si)
    combine(wedge, extras, JOIN)

    # screw path: access tunnel from the back, M2 clearance through the seat
    for (u, v) in MOUNTS:
        cyl(u, v, TUNNEL_D, far - 5, W_SEAT, CUT, wedge)
        cyl(u, v, M2_CLEAR, W_SEAT - 0.5, W_POST_TIP + 0.5, CUT, wedge)
    for (u, v) in BOSSES:
        cyl(u, v, BOSS_PILOT, -BOSS_DEPTH, 0.5, CUT, wedge)

    # USB-C: one rounded slot through the left wall (fits the plug overmold, 8 mm tall)
    usb_w = W_PCB_BACK - 1.6
    slot_side(PCB_V0 + USB_V0, PCB_V0 + USB_V1, usb_w - 4.0, usb_w + 4.0, -1.0, WALL + CLR + 0.1, CUT, wedge)

    # microSD: slot through the top wall along the card's insertion direction + finger notch
    sd_u = PCB_U0 + SD_U
    sd_w = W_PCB_BACK - 0.9
    box(sd_u - SD_W / 2 - 0.5, sd_u + SD_W / 2 + 0.5, PCB_V1 - 2, H + 20, sd_w - 2.0, sd_w + 2.0, CUT, wedge)
    slot_xy(sd_u - 10, sd_u + 10, PCB_V1 + CLR, H + 20, -13.0, 0.1, CUT, wedge)
    rear_speaker(wedge)
    appearance(wedge, "Plastic - Matte (Black)")
    return wedge, panel


def back_frame():
    """Centre and axes of the sloped back face (face frame): (centre_vw, outward normal_vw, up_vw)."""
    (v3, w3), (v2, w2) = PROFILE[3], PROFILE[2]          # base-back and top-back corners
    dv, dw = v2 - v3, w2 - w3
    L = math.hypot(dv, dw)
    up = (dv / L, dw / L)                                 # along the back face, upwards
    n = (-up[1], up[0])
    if n[1] > 0:                                          # outward = away from the screen
        n = (-n[0], -n[1])
    return ((v3 + v2) / 2, (w3 + w2) / 2), n, up


def back_face(body, n):
    """Outermost planar face parallel to the back (plane normals may point either way)."""
    best, best_d = None, None
    for f in body.faces:
        g = f.geometry
        if isinstance(g, adsk.core.Plane):
            gn = g.normal
            if abs(gn.x) < 1e-6 and abs(abs(gn.y * n[0] + gn.z * n[1]) - 1) < 1e-4:
                c = f.centroid
                d = c.y * n[0] + c.z * n[1]       # distance along the outward normal
                if best is None or d > best_d:
                    best, best_d = f, d
    if best is None:
        raise RuntimeError("back face not found")
    return best


def inner_back_plane(body, n):
    pi = root.constructionPlanes.createInput()
    pi.setByOffset(back_face(body, n), adsk.core.ValueInput.createByReal(cm(-WALL)))
    p = root.constructionPlanes.add(pi)
    p.isLightBulbOn = False
    return p


def extrude_inward(profs, dist, op, body=None):
    """Extrude against the sketch plane's normal (into the case from the back face)."""
    ei = root.features.extrudeFeatures.createInput(profs, op)
    ei.setOneSideExtent(adsk.fusion.DistanceExtentDefinition.create(adsk.core.ValueInput.createByReal(cm(dist))),
                        adsk.fusion.ExtentDirections.NegativeExtentDirection)
    if body is not None and op != NEW:
        ei.participantBodies = [body]
    return root.features.extrudeFeatures.add(ei)


def rear_speaker(body):
    """Grille through the sloped back wall + holding ring on its inside, square to the wall."""
    (cv, cw), n, up = back_frame()
    sk = root.sketches.add(back_face(body, n))
    circles = sk.sketchCurves.sketchCircles
    rows = int(GRILLE_D / GRILLE_PITCH) + 2
    for j in range(-rows, rows + 1):
        dy = j * GRILLE_PITCH * math.sqrt(3) / 2
        off = (GRILLE_PITCH / 2) if j % 2 else 0.0
        for i in range(-rows, rows + 1):
            dx = i * GRILLE_PITCH + off
            if math.hypot(dx, dy) + GRILLE_HOLE / 2 <= GRILLE_D / 2:
                circles.addByCenterRadius(P(sk, SPK_U + dx, cv + up[0] * dy, cw + up[1] * dy), cm(GRILLE_HOLE / 2))
    # a sketch on a face auto-projects the face outline: cut only the small hole profiles
    hole_area = math.pi * cm(GRILLE_HOLE / 2) ** 2
    holes = [p for p in sk.profiles if p.areaProperties().area < hole_area * 1.5]
    extrude_inward(coll(holes), WALL + 1, CUT, body)
    sk = root.sketches.add(inner_back_plane(body, n))
    c = P(sk, SPK_U, cv - n[0] * WALL, cw - n[1] * WALL)
    sk.sketchCurves.sketchCircles.addByCenterRadius(c, cm(SPK_RING_ID / 2 + SPK_RING_WALL))
    sk.sketchCurves.sketchCircles.addByCenterRadius(c, cm(SPK_RING_ID / 2))
    extrude_inward(coll([p for p in sk.profiles if p.profileLoops.count == 2]), SPK_T + 0.6, JOIN, body)


def finish_panel(panel):
    # soft front edge
    fface = planar_face_at_z(panel, PANEL_T)
    fillet(list(fface.edges), PANEL_ROUND)
    # screen window
    box(PCB_U0 + AA_LEFT - WIN_MARGIN, PCB_U0 + AA_LEFT + AA_W + WIN_MARGIN,
        PCB_V0 + AA_BOTTOM - WIN_MARGIN, PCB_V0 + AA_BOTTOM + AA_H + WIN_MARGIN,
        -1, PANEL_T + 1, CUT, panel)
    # countersunk M3 holes into the body's bosses
    for (u, v) in BOSSES:
        cyl(u, v, SCREW_D, -1, PANEL_T + 1, CUT, panel)
        sk = root.sketches.add(plane_xy(PANEL_T))
        sk.sketchCurves.sketchCircles.addByCenterRadius(P(sk, u, v, PANEL_T), cm(CSK_D / 2))
        ei = root.features.extrudeFeatures.createInput(coll(sk.profiles), CUT)
        ei.setOneSideExtent(adsk.fusion.DistanceExtentDefinition.create(
            adsk.core.ValueInput.createByReal(cm((CSK_D - SCREW_D) / 2))),
            adsk.fusion.ExtentDirections.NegativeExtentDirection,
            adsk.core.ValueInput.createByString("-45 deg"))
        ei.participantBodies = [panel]
        root.features.extrudeFeatures.add(ei)
    appearance(panel, "Plastic - Matte (Black)")


def build_refs():
    (cv, cw), n, up = back_frame()
    sk = root.sketches.add(inner_back_plane(root.bRepBodies.itemByName("Body"), n))
    sk.sketchCurves.sketchCircles.addByCenterRadius(P(sk, SPK_U, cv - n[0] * WALL, cw - n[1] * WALL), cm(SPK_D / 2))
    b = extrude_inward(coll(sk.profiles), SPK_T, NEW).bodies.item(0)
    b.name = "Speaker_REF"
    appearance(b, "Steel - Satin")
    g = box(PCB_U0, PCB_U1, PCB_V0 + 1.85, PCB_V1 - 1.85, W_PCB_BACK + 1.6, W_GLASS, NEW).bodies.item(0)
    g.name = "Glass"
    appearance(g, "Plastic - Glossy (Black)")
    p = box(PCB_U0, PCB_U1, PCB_V0, PCB_V1, W_PCB_BACK, W_PCB_BACK + 1.6, NEW).bodies.item(0)
    p.name = "PCB"
    appearance(p, "Paint - Enamel Glossy (Yellow)")
    for (u, v) in MOUNTS:
        s = cyl(u, v, STANDOFF_D, W_POST_TIP, W_PCB_BACK, NEW).bodies.item(0)
        s.name = "Standoff_REF"
        appearance(s, "Brass - Polished")


def tilt():
    """Lean the face back FACE_TILT degrees: the wedge base lands flat on the XZ plane."""
    m = adsk.core.Matrix3D.create()
    m.setToRotation(math.radians(-FACE_TILT), adsk.core.Vector3D.create(1, 0, 0),
                    adsk.core.Point3D.create(0, 0, 0))
    mi = root.features.moveFeatures.createInput2(coll(list(root.bRepBodies)))
    mi.defineAsFreeMove(m)
    root.features.moveFeatures.add(mi)


def run(_context: str):
    global design, root
    doc = app.documents.add(adsk.core.DocumentTypes.FusionDesignDocumentType)
    design = adsk.fusion.Design.cast(app.activeProduct)
    design.designType = adsk.fusion.DesignTypes.ParametricDesignType
    root = design.rootComponent
    body, panel = build_body_and_panel()
    finish_panel(panel)
    build_refs()
    tilt()
    print("Built SHORT in '%s': W=%.1f, face H=%.1f, base depth %.0f mm; board screws M2 x %d; rear speaker D%.0f"
          % (doc.name, W, H, BASE_D, SCREW_LEN, SPK_D))
