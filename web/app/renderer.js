// The renderer of the web viewer: draws the cells (or the membrane) straight from the buffers
// of the simulation. Passes: shadow map of the key light, G-buffer, ambient occlusion and its
// blur, lighting with the tone curve, and a last pass to the canvas (see wgsl.js).
import * as W from './wgsl.js';
import { lookAt, orthographic, perspective, multiply, rigidInverse, rotate, add, scale, sub, length } from './camera.js';
import { PROPERTIES } from './look.js';

const PALETTES = { ocean: 0, viridis: 1, magma: 2, diverging: 3, grey: 4 };
const G_FORMATS = ['rgba8unorm', 'rgba16float', 'rgba8unorm', 'r32float'];  // albedo + wall, normal, material, linear depth
const HDR = 'rgba16float';
const RAD = Math.PI / 180;

async function shaderModule(device, code, label) {
  const module = device.createShaderModule({ code, label });
  const info = await module.getCompilationInfo();
  const errors = info.messages.filter((m) => m.type === 'error');
  if (errors.length) throw new Error(`shader ${label}: ` + errors.map((m) => `${m.lineNum}:${m.linePos} ${m.message}`).join('; '));
  return module;
}

export class Renderer {
  static async create(device, canvas) {
    const r = new Renderer(device, canvas);
    await r.#build();
    return r;
  }

  constructor(device, canvas) {
    this.device = device;
    this.canvas = canvas;
    this.context = canvas.getContext('webgpu');
    if (!this.context) throw new Error('this canvas cannot be drawn to with WebGPU');
    this.format = navigator.gpu.getPreferredCanvasFormat();
    this.context.configure({ device, format: this.format, alphaMode: 'opaque' });
    this.frameData = new Float32Array(W.FRAME_FLOATS);
    this.shadowData = new Float32Array(W.FRAME_FLOATS);
    this.cellGroups = new WeakMap();
    this.size = [0, 0];
    this.shadowSize = 0;
  }

  async #build() {
    const d = this.device;
    const uniform = () => d.createBuffer({ size: W.FRAME_FLOATS * 4, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
    this.frameBuffer = uniform();
    this.shadowFrameBuffer = uniform();
    this.noMorph = d.createBuffer({ size: 16, usage: GPUBufferUsage.STORAGE });
    // The two draws, as indirect ones: (vertices, instances, first vertex, first instance) for
    // the cells and for the membrane (12 neighbour slots, a triangle each). The instance counts
    // are copied in from the simulation's own cell counter, on the GPU, before every frame's
    // passes: so a draw has exactly the cells there are when it runs, however many steps were
    // submitted since the page last read the count.
    this.indirect = d.createBuffer({ size: 32, usage: GPUBufferUsage.INDIRECT | GPUBufferUsage.COPY_DST });
    this.indirectData = new Uint32Array([4, 0, 0, 0, 36, 0, 0, 0]);
    d.queue.writeBuffer(this.indirect, 0, this.indirectData);
    this.sampler = d.createSampler({ magFilter: 'linear', minFilter: 'linear' });

    const VF = GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT, FS = GPUShaderStage.FRAGMENT;
    const tex = (binding, sampleType = 'float') => ({ binding, visibility: FS, texture: { sampleType } });
    this.layouts = {
      frame: d.createBindGroupLayout({ entries: [{ binding: 0, visibility: VF, buffer: { type: 'uniform' } }] }),
      cells: d.createBindGroupLayout({
        entries: [0, 1, 2, 3].map((binding) => ({ binding, visibility: GPUShaderStage.VERTEX, buffer: { type: 'read-only-storage' } })),
      }),
      ssao: d.createBindGroupLayout({ entries: [tex(0, 'unfilterable-float'), tex(1), tex(2), tex(3)] }),
      blur: d.createBindGroupLayout({ entries: [tex(0, 'unfilterable-float'), tex(1)] }),
      light: d.createBindGroupLayout({ entries: [tex(0, 'unfilterable-float'), tex(1), tex(2), tex(3), tex(4), tex(5, 'depth')] }),
      final: d.createBindGroupLayout({ entries: [tex(0), { binding: 1, visibility: FS, sampler: { type: 'filtering' } }] }),
    };
    this.frameGroup = d.createBindGroup({ layout: this.layouts.frame, entries: [{ binding: 0, resource: { buffer: this.frameBuffer } }] });
    this.shadowFrameGroup = d.createBindGroup({ layout: this.layouts.frame, entries: [{ binding: 0, resource: { buffer: this.shadowFrameBuffer } }] });

    const layout = (second) => d.createPipelineLayout({ bindGroupLayouts: [this.layouts.frame, second] });
    const gTargets = G_FORMATS.map((format) => ({ format }));
    const depthG = { format: 'depth24plus', depthWriteEnabled: true, depthCompare: 'less' };
    const depthShadow = { format: 'depth32float', depthWriteEnabled: true, depthCompare: 'less' };
    const [cells, mesh, shadowCells, shadowMesh, ssao, blur, light, final] = await Promise.all([
      shaderModule(d, W.cells, 'cells'), shaderModule(d, W.mesh, 'membrane'), shaderModule(d, W.shadowCells, 'shadow of the cells'),
      shaderModule(d, W.shadowMesh, 'shadow of the membrane'), shaderModule(d, W.ssao, 'occlusion'), shaderModule(d, W.blur, 'occlusion blur'),
      shaderModule(d, W.light, 'light'), shaderModule(d, W.final, 'final'),
    ]);
    const full = (module, second, format) => d.createRenderPipelineAsync({
      label: module.label, layout: layout(second),
      vertex: { module, entryPoint: 'vsFull' },
      fragment: { module, entryPoint: 'fs', targets: [{ format }] },
      primitive: { topology: 'triangle-list' },
    });
    const made = await Promise.all([
      d.createRenderPipelineAsync({
        label: 'cells', layout: layout(this.layouts.cells),
        vertex: { module: cells, entryPoint: 'vs' }, fragment: { module: cells, entryPoint: 'fs', targets: gTargets },
        primitive: { topology: 'triangle-strip' }, depthStencil: depthG,
      }),
      d.createRenderPipelineAsync({
        label: 'membrane', layout: layout(this.layouts.cells),
        vertex: { module: mesh, entryPoint: 'vs' }, fragment: { module: mesh, entryPoint: 'fs', targets: gTargets },
        primitive: { topology: 'triangle-list', cullMode: 'none' }, depthStencil: depthG,
      }),
      d.createRenderPipelineAsync({
        label: 'shadow of the cells', layout: layout(this.layouts.cells),
        vertex: { module: shadowCells, entryPoint: 'vs' }, fragment: { module: shadowCells, entryPoint: 'fs', targets: [] },
        primitive: { topology: 'triangle-strip' }, depthStencil: depthShadow,
      }),
      d.createRenderPipelineAsync({
        label: 'shadow of the membrane', layout: layout(this.layouts.cells),
        vertex: { module: shadowMesh, entryPoint: 'vs' },
        primitive: { topology: 'triangle-list', cullMode: 'none' }, depthStencil: depthShadow,
      }),
      full(ssao, this.layouts.ssao, HDR), full(blur, this.layouts.blur, HDR), full(light, this.layouts.light, HDR), full(final, this.layouts.final, this.format),
    ]);
    [this.pCells, this.pMesh, this.pShadowCells, this.pShadowMesh, this.pSsao, this.pBlur, this.pLight, this.pFinal] = made;
  }

  // The images of the size rendered (the canvas size times the render scale).
  #targets(w, h) {
    if (this.size[0] === w && this.size[1] === h) return;
    const d = this.device;
    for (const t of Object.values(this.tex ?? {})) t.destroy();
    const make = (format) => d.createTexture({ size: [w, h], format, usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.TEXTURE_BINDING });
    this.tex = {
      albedo: make(G_FORMATS[0]), normal: make(G_FORMATS[1]), material: make(G_FORMATS[2]), depthLinear: make(G_FORMATS[3]),
      depth: d.createTexture({ size: [w, h], format: 'depth24plus', usage: GPUTextureUsage.RENDER_ATTACHMENT }),
      gather: make(HDR), gatherBlurred: make(HDR), color: make(HDR),
    };
    this.views = Object.fromEntries(Object.entries(this.tex).map(([k, t]) => [k, t.createView()]));
    const v = this.views;
    const group = (layout, list) => d.createBindGroup({ layout, entries: list.map((resource, binding) => ({ binding, resource })) });
    this.gSsao = group(this.layouts.ssao, [v.depthLinear, v.normal, v.albedo, v.material]);
    this.gBlur = group(this.layouts.blur, [v.depthLinear, v.gather]);
    this.gFinal = group(this.layouts.final, [v.color, this.sampler]);
    this.gLight = null;
    this.size = [w, h];
  }

  #shadow(size) {
    if (this.shadowSize === size) return;
    this.shadowTex?.destroy();
    this.shadowTex = this.device.createTexture({ size: [size, size], format: 'depth32float', usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.TEXTURE_BINDING });
    this.shadowView = this.shadowTex.createView();
    this.shadowSize = size;
    this.gLight = null;
  }

  #cellGroup(b) {
    let entry = this.cellGroups.get(b.geom);
    const morph = b.morph ?? this.noMorph;
    if (!entry || entry.nrm !== b.nrm || entry.topo !== b.topo || entry.morph !== morph) {
      entry = {
        nrm: b.nrm, topo: b.topo, morph,
        group: this.device.createBindGroup({
          layout: this.layouts.cells,
          entries: [b.geom, b.nrm, b.topo, morph].map((buffer, binding) => ({ binding, resource: { buffer } })),
        }),
      };
      this.cellGroups.set(b.geom, entry);
    }
    return entry.group;
  }

  // Draws one frame.
  //   buffers    sim.renderBuffers() (or null: only the background is drawn): the per-cell
  //              buffers, and `globals` with `countOffset`, where the live cell count is
  //   look       the look settings (look.js); camera: an OrbitCamera
  //   scene      { centre, radius } of the form; step: the current step; cells: the cell count
  //   params     the parameters of the model (ranges of the colours come from them)
  draw({ buffers, look, camera, scene, step, cells, params }) {
    const d = this.device;
    const cw = Math.max(1, this.canvas.width), ch = Math.max(1, this.canvas.height);
    const limit = d.limits.maxTextureDimension2D;
    const rs = Math.min(look.scale, limit / cw, limit / ch);
    const w = Math.max(1, Math.round(cw * rs)), h = Math.max(1, Math.round(ch * rs));
    this.#targets(w, h);
    const membrane = look.draw === 'membrane';
    const cellRadius = membrane ? 0.55 : look.radius;
    const drives = [look.drive0, look.drive1, look.drive2, look.drive3];
    const translucent = look.translucency > 0 || drives[3] !== 'none';
    const glowing = look.emission > 0;
    const haveCells = !!buffers && (!!buffers.globals || cells > 0);
    const wantShadow = haveCells && ((look.shadows && look.shadowStrength > 0) || translucent);
    this.#shadow(wantShadow ? Number(look.shadowSize) : this.shadowSize || 1024);
    if (!this.gLight) {
      const v = this.views;
      this.gLight = d.createBindGroup({
        layout: this.layouts.light,
        entries: [v.depthLinear, v.normal, v.albedo, v.material, v.gatherBlurred, this.shadowView].map((resource, binding) => ({ binding, resource })),
      });
    }

    // --- the frame's uniforms (makeUniforms of the desktop renderer)
    const F = this.frameData, O = W.FRAME_OFFSET;
    const put = (name, values) => F.set(values, O[name]);
    const view = camera.view(), invView = rigidInverse(view);
    const eye = camera.eye();
    const reach = scene.radius * 1.05 + cellRadius;
    const toCentre = length(sub(eye, scene.centre));
    const zFar = toCentre + reach;
    const zNear = Math.max(toCentre - reach, zFar * 2e-4);
    const proj = perspective(camera.fovy, w / h, zNear, zFar);
    put('view', view);
    put('proj', proj);
    put('invView', invView);
    put('viewport', [w, h, 1 / w, 1 / h]);
    put('sphere', [cellRadius, 0.6 * rs, 0, 0]);
    put('color', [look.colorBy === 'material' ? 0 : PROPERTIES[look.colorBy] ?? 0, 0, 0, step]);

    // The key light: relative to the camera, or fixed in the world.
    const az = look.lightAz * RAD, el = look.lightEl * RAD;
    const dir = [Math.cos(el) * Math.sin(az), Math.sin(el), Math.cos(el) * Math.cos(az)];
    const lightView = look.lightFollow ? dir : rotate(view, dir);
    const lightWorld = look.lightFollow ? rotate(invView, dir) : dir;
    put('light', [...lightView, Math.min(Math.max(look.ambient, 0), 1)]);
    put('lightColor', [...look.lightColor.map((c) => c * look.lightIntensity), look.shadows ? Math.min(Math.max(look.shadowStrength, 0), 1) : 0]);
    put('sky', [...look.skyColor, 0]);
    put('ground', [...look.groundColor, 0]);

    // The view of the key light for the shadow map: an orthographic box around the form.
    const half = scene.radius * 1.05 + cellRadius;
    const up = Math.abs(lightWorld[1]) > 0.95 ? [1, 0, 0] : [0, 1, 0];
    const lightV = lookAt(add(scene.centre, scale(lightWorld, 2 * half)), scene.centre, up);
    const lightP = orthographic(half, 0.5 * half, 3.5 * half);
    const mapSize = this.shadowSize;
    const shadowTexel = (2 * half) / mapSize;
    put('lightViewProj', multiply(lightP, lightV));
    put('shadow', [0.35 * cellRadius + 0.5 * shadowTexel, look.shadowSoftness, 1 / mapSize, 1 / mapSize]);
    put('shadow2', [3 * half, wantShadow ? 1 : 0, mapSize, 0]);

    put('ao', [look.aoSmall, look.aoLarge, look.aoIntensity, look.ao ? 1 : 0]);
    put('baseColor', [...look.baseColor, look.exposure]);
    put('background', [...look.background, look.vignette]);
    put('palette', [PALETTES[look.palette] ?? 0, look.paletteReverse ? 1 : 0, look.gradient, 0]);
    put('matBase', [look.roughness, look.metal, look.specular, look.sheen]);
    put('matSss', [look.subsurface, look.sssRadius, look.translucency, Math.max(look.transDepth, 1e-3)]);
    put('matSssColor', [...look.sssColor.map((c) => Math.min(Math.max(c, 0.02), 1)), 0]);
    put('matEmission', [...look.emissionColor, Math.max(look.emission, 0)]);
    put('matEmission2', [look.emissionTint, look.emissionLights ? 1 : 0, 0, 0]);
    put('modSource', drives.map((p) => PROPERTIES[p] ?? 0));
    put('modLo', [look.lo0, look.lo1, look.lo2, look.lo3]);
    put('modHi', [look.hi0, look.hi1, look.hi2, look.hi3]);
    put('modFrom', [look.from0, look.from1, look.from2, look.from3]);
    put('modTo', [look.to0, look.to1, look.to2, look.to3]);
    // The usual range of every cell property (shadingFor of the desktop viewer). Age: about the
    // time a newborn cell needs to divide again. Generation: about log2 of the growth.
    const meanFood = Math.max(0.5 * (params.foodMin + params.foodMax), 1e-9);
    put('propMin0', [0, 0, 0, 0]);
    put('propMin1', [-0.6, 0, 0, 0]);
    put('propMax0', [Math.min(Math.max((0.5 * params.foodThreshold) / meanFood, 1), 1e7), params.foodThreshold, Math.max(step, 1),
      Math.max(4, Math.log2(Math.max(cells, 162) / 162) + 6)]);
    put('propMax1', [0.6, 1, 0, 0]);
    put('wall', [...look.wallColor, membrane && look.walls ? look.wallStrength : 0]);
    const needsCurvature = look.colorBy === 'curvature' || drives.includes('curvature');
    put('style', [Math.max(look.wallWidth, 0.25) * rs, buffers?.morph ? 1 : 0, params.linkRestLength ?? 1, needsCurvature ? 1 : 0]);
    put('env', [look.env ? 1 : 0, look.envIntensity, 0, 0]);
    put('misc', [look.fxaa ? 1 : 0, 0, 0, 0]);
    d.queue.writeBuffer(this.frameBuffer, 0, F);

    const encoder = d.createCommandEncoder();
    const cellGroup = haveCells ? this.#cellGroup(buffers) : null;
    const drawCells = (pass, cellsPipeline, meshPipeline, frameGroup) => {
      pass.setPipeline(membrane ? meshPipeline : cellsPipeline);
      pass.setBindGroup(0, frameGroup);
      pass.setBindGroup(1, cellGroup);
      pass.drawIndirect(this.indirect, membrane ? 16 : 0);
    };
    if (haveCells) {
      if (buffers.globals) {
        encoder.copyBufferToBuffer(buffers.globals, buffers.countOffset, this.indirect, 4, 4);
        encoder.copyBufferToBuffer(buffers.globals, buffers.countOffset, this.indirect, 20, 4);
      } else {
        // A simulation that does not hand out its counter: the count as the page last read it
        // (never more than there are, so the newest cells show a moment late).
        this.indirectData[1] = this.indirectData[5] = cells;
        d.queue.writeBuffer(this.indirect, 0, this.indirectData);
      }
    }

    if (wantShadow) {
      const S = this.shadowData;
      S.set(F);
      S.set(lightV, O.view);
      S.set(lightP, O.proj);
      S.set([cellRadius, 0.7 * shadowTexel, 0, 0], O.sphere);
      d.queue.writeBuffer(this.shadowFrameBuffer, 0, S);
      const pass = encoder.beginRenderPass({
        colorAttachments: [],
        depthStencilAttachment: { view: this.shadowView, depthClearValue: 1, depthLoadOp: 'clear', depthStoreOp: 'store' },
      });
      drawCells(pass, this.pShadowCells, this.pShadowMesh, this.shadowFrameGroup);
      pass.end();
    }

    {
      const v = this.views;
      const clear = { r: 0, g: 0, b: 0, a: 0 };
      const pass = encoder.beginRenderPass({
        colorAttachments: [v.albedo, v.normal, v.material, v.depthLinear].map((view) => ({ view, clearValue: clear, loadOp: 'clear', storeOp: 'store' })),
        depthStencilAttachment: { view: v.depth, depthClearValue: 1, depthLoadOp: 'clear', depthStoreOp: 'discard' },
      });
      if (haveCells) drawCells(pass, this.pCells, this.pMesh, this.frameGroup);
      pass.end();
    }

    const fullPass = (view, pipeline, group) => {
      const pass = encoder.beginRenderPass({ colorAttachments: [{ view, loadOp: 'clear', clearValue: { r: 0, g: 0, b: 0, a: 1 }, storeOp: 'store' }] });
      pass.setPipeline(pipeline);
      pass.setBindGroup(0, this.frameGroup);
      pass.setBindGroup(1, group);
      pass.draw(3);
      pass.end();
    };
    if (haveCells && (look.ao || (glowing && look.emissionLights))) {
      fullPass(this.views.gather, this.pSsao, this.gSsao);
      fullPass(this.views.gatherBlurred, this.pBlur, this.gBlur);
    }
    fullPass(this.views.color, this.pLight, this.gLight);
    fullPass(this.context.getCurrentTexture().createView(), this.pFinal, this.gFinal);
    d.queue.submit([encoder.finish()]);
  }

  // The picture on the canvas as a PNG. Call right after draw(), in the same task.
  png() {
    return new Promise((resolve, reject) => this.canvas.toBlob((blob) => (blob ? resolve(blob) : reject(new Error('the picture could not be read from the canvas'))), 'image/png'));
  }

  destroy() {
    for (const t of Object.values(this.tex ?? {})) t.destroy();
    this.shadowTex?.destroy();
    this.frameBuffer?.destroy();
    this.shadowFrameBuffer?.destroy();
    this.noMorph?.destroy();
    this.indirect?.destroy();
  }
}
