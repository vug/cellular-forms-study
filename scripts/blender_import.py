"""Import Cellular Forms PLY exports into Blender and (optionally) render them with Cycles.

    cf_export run\\latest.cfs --points cells.ply --mesh membrane.ply
    blender --background --python scripts/blender_import.py -- --points cells.ply --render cells.png
    blender --background --python scripts/blender_import.py -- --mesh membrane.ply --render membrane.png
    blender --python scripts/blender_import.py -- --points cells.ply          (opens the scene to work on)

Options after the "--":
    --points <file>     cells as spheres: the PLY's `radius` attribute is the sphere radius
    --mesh <file>       the membrane as a smooth-shaded mesh with subsurface scattering
    --color <name>      attribute to colour by: age (default), food, generation, or none
    --render <png>      render one image and (in background mode) exit
    --size <W>x<H>      image size (default 1600x1200)
    --samples <n>       Cycles samples (default 96)
    --save <file.blend> save the scene
    --cpu               render on the CPU (default: the GPU when Cycles can use one)

Written for Blender 4.2 and later (tested with 5.2). The cells are a point cloud made by a
Geometry Nodes "Mesh to Points" node, which Cycles renders as true spheres, so tens of
millions of cells need no instanced geometry.
"""
import math
import os
import sys
import warnings

import bpy
from mathutils import Vector


def parse_args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    opts = {"points": None, "mesh": None, "color": "age", "render": None, "size": "1600x1200", "samples": 96, "save": None, "cpu": False}
    i = 0
    while i < len(argv):
        key = argv[i].lstrip("-")
        if key == "cpu":
            opts["cpu"] = True
            i += 1
        elif key in opts and i + 1 < len(argv):
            opts[key] = argv[i + 1]
            i += 2
        else:
            raise SystemExit(f"blender_import.py: unknown or incomplete option {argv[i]}")
    opts["samples"] = int(opts["samples"])
    # Blender resolves relative paths against the .blend file, not the shell's directory.
    for key in ("points", "mesh", "render", "save"):
        if opts[key]:
            opts[key] = os.path.abspath(opts[key])
    return opts


def use_nodes(datablock):
    # Needed before Blender 5; from 5.0 node trees are always on and the property is deprecated.
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        datablock.use_nodes = True


def import_ply(path):
    before = set(bpy.data.objects)
    # The simulation is Y-up (the default light comes from +Y); Blender is Z-up.
    bpy.ops.wm.ply_import(filepath=path, forward_axis='NEGATIVE_Z', up_axis='Y')
    new = [o for o in bpy.data.objects if o not in before]
    if not new:
        raise SystemExit(f"blender_import.py: nothing was imported from {path}")
    return new[0]


def attribute_range(obj, name):
    attr = obj.data.attributes.get(name)
    if attr is None or len(attr.data) == 0:
        return None
    values = [0.0] * len(attr.data)
    attr.data.foreach_get("value", values)
    values.sort()
    # 2nd to 98th percentile, so a few outliers do not flatten the colours
    return values[len(values) // 50], values[len(values) * 49 // 50]


def make_material(name, obj, color_by, subsurface):
    mat = bpy.data.materials.new(name)
    use_nodes(mat)
    nodes, links = mat.node_tree.nodes, mat.node_tree.links
    bsdf = nodes.get("Principled BSDF")
    bsdf.inputs["Roughness"].default_value = 0.55
    base = (0.80, 0.72, 0.62, 1.0)
    bsdf.inputs["Base Color"].default_value = base
    if subsurface:
        for socket, value in (("Subsurface Weight", 0.6), ("Subsurface Scale", 0.8)):
            if socket in bsdf.inputs:
                bsdf.inputs[socket].default_value = value
        if "Subsurface Radius" in bsdf.inputs:
            bsdf.inputs["Subsurface Radius"].default_value = (1.0, 0.45, 0.3)
    rng = attribute_range(obj, color_by) if color_by != "none" else None
    if rng is not None and rng[1] > rng[0]:
        attr = nodes.new("ShaderNodeAttribute")
        attr.attribute_name = color_by
        attr.attribute_type = 'GEOMETRY'
        remap = nodes.new("ShaderNodeMapRange")
        remap.inputs["From Min"].default_value = rng[0]
        remap.inputs["From Max"].default_value = rng[1]
        ramp = nodes.new("ShaderNodeValToRGB")
        # young / low values warm and bright, old / high values darker and cooler
        ramp.color_ramp.elements[0].color = (0.95, 0.80, 0.55, 1.0)
        ramp.color_ramp.elements[1].color = (0.28, 0.33, 0.45, 1.0)
        links.new(attr.outputs["Fac"], remap.inputs["Value"])
        links.new(remap.outputs["Result"], ramp.inputs["Fac"])
        links.new(ramp.outputs["Color"], bsdf.inputs["Base Color"])
    return mat


def cells_as_spheres(obj, material):
    """Geometry Nodes: every vertex becomes a point with the radius stored in the PLY."""
    group = bpy.data.node_groups.new("Cells as spheres", 'GeometryNodeTree')
    group.interface.new_socket(name="Geometry", in_out='INPUT', socket_type='NodeSocketGeometry')
    group.interface.new_socket(name="Geometry", in_out='OUTPUT', socket_type='NodeSocketGeometry')
    nodes, links = group.nodes, group.links
    n_in = nodes.new("NodeGroupInput")
    n_out = nodes.new("NodeGroupOutput")
    radius = nodes.new("GeometryNodeInputNamedAttribute")
    radius.data_type = 'FLOAT'
    radius.inputs["Name"].default_value = "radius"
    to_points = nodes.new("GeometryNodeMeshToPoints")
    set_material = nodes.new("GeometryNodeSetMaterial")
    set_material.inputs["Material"].default_value = material
    links.new(n_in.outputs[0], to_points.inputs["Mesh"])
    links.new(radius.outputs["Attribute"], to_points.inputs["Radius"])
    links.new(to_points.outputs["Points"], set_material.inputs["Geometry"])
    links.new(set_material.outputs["Geometry"], n_out.inputs[0])
    modifier = obj.modifiers.new("Cells as spheres", 'NODES')
    modifier.node_group = group


def bounds(objects):
    lo = Vector((math.inf,) * 3)
    hi = Vector((-math.inf,) * 3)
    for obj in objects:
        for corner in obj.bound_box:
            p = obj.matrix_world @ Vector(corner)
            lo = Vector(map(min, lo, p))
            hi = Vector(map(max, hi, p))
    return lo, hi


def setup_scene(objects, opts):
    scene = bpy.context.scene
    lo, hi = bounds(objects)
    centre = (lo + hi) / 2
    radius = max(max(hi - lo) / 2, 1.0)  # half the largest extent: the form fills most of the frame

    cam_data = bpy.data.cameras.new("Camera")
    cam_data.lens = 70
    cam = bpy.data.objects.new("Camera", cam_data)
    scene.collection.objects.link(cam)
    width, height = (int(x) for x in opts["size"].lower().split("x"))
    cam_data.sensor_fit = 'VERTICAL'
    fov_v = 2 * math.atan(cam_data.sensor_height / 2 / cam_data.lens)
    fov_h = 2 * math.atan(math.tan(fov_v / 2) * width / height)
    distance = radius / math.tan(min(fov_v, fov_h) / 2) * 1.25
    direction = Vector((0.55, -0.80, 0.30)).normalized()
    cam.location = centre + direction * distance
    cam.rotation_euler = (centre - cam.location).to_track_quat('-Z', 'Y').to_euler()
    cam_data.clip_start = max(distance - 2 * radius, distance * 0.01)
    cam_data.clip_end = distance + 2 * radius
    scene.camera = cam

    sun_data = bpy.data.lights.new("Key", 'SUN')
    sun_data.energy = 3.0
    sun_data.angle = math.radians(8)
    sun = bpy.data.objects.new("Key", sun_data)
    scene.collection.objects.link(sun)
    sun.rotation_euler = (math.radians(50), 0.0, math.radians(35))

    world = scene.world or bpy.data.worlds.new("World")
    scene.world = world
    use_nodes(world)
    background = world.node_tree.nodes.get("Background")
    if background:
        background.inputs["Color"].default_value = (0.55, 0.60, 0.68, 1.0)
        background.inputs["Strength"].default_value = 0.9

    scene.render.resolution_x = width
    scene.render.resolution_y = height
    scene.render.resolution_percentage = 100
    scene.render.engine = 'CYCLES'
    scene.cycles.samples = opts["samples"]
    scene.cycles.use_denoising = True
    scene.render.film_transparent = False
    scene.render.image_settings.file_format = 'PNG'

    device = "CPU"
    if not opts["cpu"]:
        try:
            prefs = bpy.context.preferences.addons["cycles"].preferences
            for kind in ("OPTIX", "CUDA", "HIP", "ONEAPI", "METAL"):
                try:
                    prefs.compute_device_type = kind
                except TypeError:
                    continue
                prefs.get_devices()
                gpus = [d for d in prefs.devices if d.type == kind]
                if gpus:
                    for d in prefs.devices:
                        d.use = d.type == kind
                    scene.cycles.device = 'GPU'
                    device = f"{kind} ({gpus[0].name})"
                    break
        except Exception as error:  # no Cycles GPU support in this build: render on the CPU
            print("blender_import.py: GPU setup failed, using the CPU:", error)
    return device


def main():
    opts = parse_args()
    if not opts["points"] and not opts["mesh"]:
        raise SystemExit("blender_import.py: give --points and/or --mesh (after a -- separator)")
    # Start from an empty scene, whatever the startup file holds.
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)

    objects = []
    if opts["points"]:
        cells = import_ply(opts["points"])
        cells.name = "Cells"
        material = make_material("Cells", cells, opts["color"], subsurface=False)
        cells_as_spheres(cells, material)
        objects.append(cells)
        print(f"blender_import.py: {len(cells.data.vertices)} cells from {opts['points']}; attributes: "
              + ", ".join(a.name for a in cells.data.attributes))
    if opts["mesh"]:
        membrane = import_ply(opts["mesh"])
        membrane.name = "Membrane"
        membrane.data.materials.append(make_material("Membrane", membrane, opts["color"], subsurface=True))
        for polygon in membrane.data.polygons:
            polygon.use_smooth = True
        objects.append(membrane)
        print(f"blender_import.py: membrane with {len(membrane.data.vertices)} vertices and {len(membrane.data.polygons)} faces from {opts['mesh']}")

    device = setup_scene(objects, opts)
    if opts["save"]:
        bpy.ops.wm.save_as_mainfile(filepath=opts["save"])
        print("blender_import.py: saved", opts["save"])
    if opts["render"]:
        bpy.context.scene.render.filepath = opts["render"]
        bpy.ops.render.render(write_still=True)
        print(f"blender_import.py: rendered {opts['render']} with Cycles on {device}, {opts['samples']} samples")


main()
