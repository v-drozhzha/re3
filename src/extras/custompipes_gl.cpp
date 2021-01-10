#include "common.h"

#ifdef RW_OPENGL
#ifdef EXTENDED_PIPELINES

#include "rpmatfx.h"

#include "main.h"
#include "RwHelper.h"
#include "Lights.h"
#include "Timecycle.h"
#include "FileMgr.h"
#include "Clock.h"
#include "Weather.h"
#include "TxdStore.h"
#include "Renderer.h"
#include "World.h"
#include "custompipes.h"

#ifndef LIBRW
#error "Need librw for EXTENDED_PIPELINES"
#endif

namespace CustomPipes {

static int32 u_viewVec;
static int32 u_rampStart;
static int32 u_rampEnd;
static int32 u_rimData;

static int32 u_lightMap;

static int32 u_eye;
static int32 u_reflProps;
static int32 u_specDir;
static int32 u_specColor;

static int32 u_amb;
static int32 u_emiss;
static int32 u_colorscale;

static int32 u_texMatrix;
static int32 u_fxparams;

#define U(i) currentShader->uniformLocations[i]

/*
 * Leeds & Neo Vehicle pipe
 */

rw::gl3::Shader *leedsVehicleShader_add;
rw::gl3::Shader *leedsVehicleShader_blend;

rw::gl3::Shader *neoVehicleShader;

static rw::RawMatrix normal2texcoord_flipU = {
	{ -0.5f,  0.0f, 0.0f }, 0.0f,
	{ 0.0f, -0.5f, 0.0f }, 0.0f,
	{ 0.0f,  0.0f, 1.0f }, 0.0f,
	{ 0.5f,  0.5f, 0.0f }, 1.0f
};

static void
uploadEnvMatrix(rw::Frame *frame)
{
	using namespace rw;
	using namespace rw::gl3;

	Matrix invMat;
	if(frame == nil)
		frame = engine->currentCamera->getFrame();

	// cache the matrix across multiple meshes
	static RawMatrix envMtx;
// can't do it, frame matrix may change
//	if(frame != lastEnvFrame){
//		lastEnvFrame = frame;
	{

		Matrix tmp = *frame->getLTM();
		// Now the weird part: we remove the camera pitch
		tmp.at.z = 0.0f;
		tmp.at = normalize(tmp.at);
		tmp.right.x = -tmp.at.y;
		tmp.right.y = tmp.at.x;
		tmp.right.z = 0.0f;;
		tmp.up.set(0.0f, 0.0f, 1.0f);
		tmp.pos.set(0.0f, 0.0f, 0.0f);
		tmp.flags = Matrix::TYPEORTHONORMAL;

		RawMatrix invMtx;
		Matrix::invert(&invMat, &tmp);
		convMatrix(&invMtx, &invMat);
		RawMatrix::mult(&envMtx, &invMtx, &normal2texcoord_flipU);
	}
	glUniformMatrix4fv(U(u_texMatrix), 1, GL_FALSE, (float*)&envMtx);
}

static void
leedsVehicleRenderCB(rw::Atomic *atomic, rw::gl3::InstanceDataHeader *header)
{
	using namespace rw;
	using namespace rw::gl3;

	Material *m;

	setWorldMatrix(atomic->getFrame()->getLTM());
	lightingCB(atomic);

#ifdef RW_GL_USE_VAOS
	glBindVertexArray(header->vao);
#else
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, header->ibo);
	glBindBuffer(GL_ARRAY_BUFFER, header->vbo);
	setAttribPointers(header->attribDesc, header->numAttribs);
#endif

	InstanceData *inst = header->inst;
	rw::int32 n = header->numMeshes;

	if(bChromeCheat)
		leedsVehicleShader_blend->use();
	else
		leedsVehicleShader_add->use();

	setTexture(1, EnvMapTex);
	uploadEnvMatrix(nil);

	SetRenderState(SRCBLEND, BLENDONE);

	while(n--){
		m = inst->material;

		rw::SetRenderState(VERTEXALPHA, inst->vertexAlpha || m->color.alpha != 0xFF);

		float coef = 0.0f;
		if(RpMatFXMaterialGetEffects(m) == rpMATFXEFFECTENVMAP)
			coef = RpMatFXMaterialGetEnvMapCoefficient(m);
		coef *= 0.5f;
		if(bChromeCheat && coef > 0.0f)
			coef = 1.0f;
		glUniform1f(U(u_fxparams), coef);

		setMaterial(m->color, m->surfaceProps);

		setTexture(0, m->texture);

		drawInst(header, inst);
		inst++;
	}

	setTexture(1, nil);

	SetRenderState(SRCBLEND, BLENDSRCALPHA);

#ifndef RW_GL_USE_VAOS
	disableAttribPointers(header->attribDesc, header->numAttribs);
#endif
}


static void
uploadSpecLights(void)
{
	using namespace rw::gl3;

	rw::RGBAf colors[1 + NUMEXTRADIRECTIONALS];
	struct {
		rw::V3d dir;
		float power;
	} dirs[1 + NUMEXTRADIRECTIONALS];
	memset(colors, 0, sizeof(colors));
	memset(dirs, 0, sizeof(dirs));
	for(int i = 0; i < 1+NUMEXTRADIRECTIONALS; i++)
		dirs[i].power = 1.0f;
	float power = Power.Get();
	Color speccol = SpecColor.Get();
	colors[0].red = speccol.r;
	colors[0].green = speccol.g;
	colors[0].blue = speccol.b;
	dirs[0].dir = pDirect->getFrame()->getLTM()->at;
	dirs[0].power = power;
	for(int i = 0; i < NUMEXTRADIRECTIONALS; i++){
		if(pExtraDirectionals[i]->getFlags() & rw::Light::LIGHTATOMICS){
			colors[1+i] = pExtraDirectionals[i]->color;
			dirs[1+i].dir = pExtraDirectionals[i]->getFrame()->getLTM()->at;
			dirs[1+i].power = power*2.0f;
		}
	}
	glUniform4fv(U(u_specDir), 1 + NUMEXTRADIRECTIONALS, (float*)&dirs);
	glUniform4fv(U(u_specColor), 1 + NUMEXTRADIRECTIONALS, (float*)&colors);
}

static void
vehicleRenderCB(rw::Atomic *atomic, rw::gl3::InstanceDataHeader *header)
{
	using namespace rw;
	using namespace rw::gl3;

	// TODO: make this less of a kludge
	if(VehiclePipeSwitch == VEHICLEPIPE_MATFX){
		leedsVehicleRenderCB(atomic, header);
//		matFXGlobals.pipelines[rw::platform]->render(atomic);
		return;
	}

	Material *m;

	setWorldMatrix(atomic->getFrame()->getLTM());
	lightingCB(atomic);

#ifdef RW_GL_USE_VAOS
	glBindVertexArray(header->vao);
#else
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, header->ibo);
	glBindBuffer(GL_ARRAY_BUFFER, header->vbo);
	setAttribPointers(header->attribDesc, header->numAttribs);
#endif

	InstanceData *inst = header->inst;
	rw::int32 n = header->numMeshes;

	neoVehicleShader->use();

	V3d eyePos = rw::engine->currentCamera->getFrame()->getLTM()->pos;
	glUniform3fv(U(u_eye), 1, (float*)&eyePos);

	uploadSpecLights();

	float reflProps[4];
	reflProps[0] = Fresnel.Get();
	reflProps[1] = SpecColor.Get().a;

	setTexture(1, EnvMapTex);

	SetRenderState(SRCBLEND, BLENDONE);

	while(n--){
		m = inst->material;

		setMaterial(m->color, m->surfaceProps);

		setTexture(0, m->texture);

		rw::SetRenderState(VERTEXALPHA, inst->vertexAlpha || m->color.alpha != 0xFF);

		reflProps[2] = m->surfaceProps.specular * VehicleShininess;
		reflProps[3] = m->surfaceProps.specular == 0.0f ? 0.0f : VehicleSpecularity;
		glUniform4fv(U(u_reflProps), 1, reflProps);

		drawInst(header, inst);
		inst++;
	}

	setTexture(1, nil);

	SetRenderState(SRCBLEND, BLENDSRCALPHA);

#ifndef RW_GL_USE_VAOS
	disableAttribPointers(header->attribDesc, header->numAttribs);
#endif
}

void
CreateVehiclePipe(void)
{
	using namespace rw;
	using namespace rw::gl3;

	if(CFileMgr::LoadFile("neo/carTweakingTable.dat", work_buff, sizeof(work_buff), "r") <= 0)
		printf("Error: couldn't open 'neo/carTweakingTable.dat'\n");
	else{
		char *fp = (char*)work_buff;
		fp = ReadTweakValueTable(fp, Fresnel);
		fp = ReadTweakValueTable(fp, Power);
		fp = ReadTweakValueTable(fp, DiffColor);
		fp = ReadTweakValueTable(fp, SpecColor);
	}


	{
#include "shaders/neoVehicle_fs_gl.inc"
#include "shaders/neoVehicle_vs_gl.inc"
	const char *vs[] = { shaderDecl, header_vert_src, neoVehicle_vert_src, nil };
	const char *fs[] = { shaderDecl, header_frag_src, neoVehicle_frag_src, nil };
	neoVehicleShader = Shader::create(vs, fs);
	assert(neoVehicleShader);
	}

	{
#include "shaders/leedsVehicle_add_gl.inc"
#include "shaders/leedsVehicle_blend_gl.inc"
#include "shaders/leedsVehicle_vs_gl.inc"
	const char *vs[] = { shaderDecl, header_vert_src, leedsVehicle_vert_src, nil };
	const char *fs_add[] = { shaderDecl, header_frag_src, leedsVehicle_add_frag_src, nil };
	const char *fs_blend[] = { shaderDecl, header_frag_src, leedsVehicle_blend_frag_src, nil };
	leedsVehicleShader_add = Shader::create(vs, fs_add);
	assert(leedsVehicleShader_add);
	leedsVehicleShader_blend = Shader::create(vs, fs_blend);
	assert(leedsVehicleShader_blend);
	}


	rw::gl3::ObjPipeline *pipe = rw::gl3::ObjPipeline::create();
	pipe->instanceCB = rw::gl3::defaultInstanceCB;
	pipe->uninstanceCB = nil;
	pipe->renderCB = vehicleRenderCB;
	vehiclePipe = pipe;
}

void
DestroyVehiclePipe(void)
{
	neoVehicleShader->destroy();
	neoVehicleShader = nil;

	leedsVehicleShader_add->destroy();
	leedsVehicleShader_add = nil;

	leedsVehicleShader_blend->destroy();
	leedsVehicleShader_blend = nil;

	((rw::gl3::ObjPipeline*)vehiclePipe)->destroy();
	vehiclePipe = nil;
}



/*
 * Leeds World pipe
 */

rw::gl3::Shader *leedsWorldShader;
rw::gl3::Shader *leedsWorldShader_mobile;

static void
worldRenderCB(rw::Atomic *atomic, rw::gl3::InstanceDataHeader *header)
{
	using namespace rw;
	using namespace rw::gl3;

	Material *m;

	setWorldMatrix(atomic->getFrame()->getLTM());

#ifdef RW_GL_USE_VAOS
	glBindVertexArray(header->vao);
#else
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, header->ibo);
	glBindBuffer(GL_ARRAY_BUFFER, header->vbo);
	setAttribPointers(header->attribDesc, header->numAttribs);
#endif

	InstanceData *inst = header->inst;
	rw::int32 n = header->numMeshes;

	if(CustomPipes::WorldPipeSwitch == CustomPipes::WORLDPIPE_MOBILE)
		CustomPipes::leedsWorldShader_mobile->use();
	else
		CustomPipes::leedsWorldShader->use();

	RGBAf amb, emiss;
	amb.red = CTimeCycle::GetAmbientRed();
	amb.green = CTimeCycle::GetAmbientGreen();
	amb.blue = CTimeCycle::GetAmbientBlue();
	amb.alpha = 1.0f;
	emiss = pAmbient->color;

	glUniform4fv(U(u_amb), 1, (float*)&amb);
	glUniform4fv(U(u_emiss), 1, (float*)&emiss);

	float colorscale[4];
	colorscale[3] = 1.0f;

	while(n--){
		m = inst->material;

		float cs = 1.0f;
		if(WorldPipeSwitch == WORLDPIPE_PS2 && m->texture)
			cs = 255/128.0f;
		colorscale[0] = colorscale[1] = colorscale[2] = cs;
		glUniform4fv(U(u_colorscale), 1, colorscale);

		setTexture(0, m->texture);

		setMaterial(m->color, m->surfaceProps, 0.5f);

		rw::SetRenderState(VERTEXALPHA, inst->vertexAlpha || m->color.alpha != 0xFF);

		drawInst(header, inst);
		inst++;
	}
#ifndef RW_GL_USE_VAOS
	disableAttribPointers(header->attribDesc, header->numAttribs);
#endif
}

void
CreateWorldPipe(void)
{
	using namespace rw;
	using namespace rw::gl3;

//	if(CFileMgr::LoadFile("neo/worldTweakingTable.dat", work_buff, sizeof(work_buff), "r") <= 0)
//		printf("Error: couldn't open 'neo/worldTweakingTable.dat'\n");
//	else
//		ReadTweakValueTable((char*)work_buff, WorldLightmapBlend);

	{
#include "shaders/scale_fs_gl.inc"
#include "shaders/leedsBuilding_vs_gl.inc"
#include "shaders/leedsBuilding_mobile_vs_gl.inc"
	const char *vs[] = { shaderDecl, header_vert_src, leedsBuilding_vert_src, nil };
	const char *vs_mobile[] = { shaderDecl, header_vert_src, leedsBuilding_mobile_vert_src, nil };
	const char *fs[] = { shaderDecl, header_frag_src, scale_frag_src, nil };
	leedsWorldShader = Shader::create(vs, fs);
	assert(leedsWorldShader);
	leedsWorldShader_mobile = Shader::create(vs_mobile, fs);
	assert(leedsWorldShader_mobile);
	}


	rw::gl3::ObjPipeline *pipe = rw::gl3::ObjPipeline::create();
	pipe->instanceCB = rw::gl3::defaultInstanceCB;
	pipe->uninstanceCB = nil;
	pipe->renderCB = worldRenderCB;
	worldPipe = pipe;
}

void
DestroyWorldPipe(void)
{
	leedsWorldShader->destroy();
	leedsWorldShader = nil;
	leedsWorldShader_mobile->destroy();
	leedsWorldShader_mobile = nil;

	((rw::gl3::ObjPipeline*)worldPipe)->destroy();
	worldPipe = nil;
}




/*
 * Neo Gloss pipe
 */

rw::gl3::Shader *neoGlossShader;

static void
glossRenderCB(rw::Atomic *atomic, rw::gl3::InstanceDataHeader *header)
{
	using namespace rw;
	using namespace rw::gl3;

	worldRenderCB(atomic, header);
	if(!GlossEnable)
		return;

	Material *m;

#ifdef RW_GL_USE_VAOS
	glBindVertexArray(header->vao);
#else
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, header->ibo);
	glBindBuffer(GL_ARRAY_BUFFER, header->vbo);
	setAttribPointers(header->attribDesc, header->numAttribs);
#endif

	InstanceData *inst = header->inst;
	rw::int32 n = header->numMeshes;

	neoGlossShader->use();

	V3d eyePos = rw::engine->currentCamera->getFrame()->getLTM()->pos;
	glUniform3fv(U(u_eye), 1, (float*)&eyePos);
	glUniform4fv(U(u_reflProps), 1, (float*)&GlossMult);

	SetRenderState(VERTEXALPHA, TRUE);
	SetRenderState(SRCBLEND, BLENDONE);
	SetRenderState(DESTBLEND, BLENDONE);
	SetRenderState(ZWRITEENABLE, FALSE);
	SetRenderState(ALPHATESTFUNC, ALPHAALWAYS);

	while(n--){
		m = inst->material;

		RGBA color = { 255, 255, 255, m->color.alpha };
		setMaterial(color, m->surfaceProps);

		if(m->texture){
			Texture *tex = GetGlossTex(m);
			if(tex){
				setTexture(0, tex);
				drawInst(header, inst);
			}
		}
		inst++;
	}

	SetRenderState(ZWRITEENABLE, TRUE);
	SetRenderState(ALPHATESTFUNC, ALPHAGREATEREQUAL);
	SetRenderState(SRCBLEND, BLENDSRCALPHA);
	SetRenderState(DESTBLEND, BLENDINVSRCALPHA);

#ifndef RW_GL_USE_VAOS
	disableAttribPointers(header->attribDesc, header->numAttribs);
#endif
}

void
CreateGlossPipe(void)
{
	using namespace rw;
	using namespace rw::gl3;

	{
#include "shaders/neoGloss_fs_gl.inc"
#include "shaders/neoGloss_vs_gl.inc"
	const char *vs[] = { shaderDecl, header_vert_src, neoGloss_vert_src, nil };
	const char *fs[] = { shaderDecl, header_frag_src, neoGloss_frag_src, nil };
	neoGlossShader = Shader::create(vs, fs);
	assert(neoGlossShader);
	}

	rw::gl3::ObjPipeline *pipe = rw::gl3::ObjPipeline::create();
	pipe->instanceCB = rw::gl3::defaultInstanceCB;
	pipe->uninstanceCB = nil;
	pipe->renderCB = glossRenderCB;
	glossPipe = pipe;
}

void
DestroyGlossPipe(void)
{
	neoGlossShader->destroy();
	neoGlossShader = nil;

	((rw::gl3::ObjPipeline*)glossPipe)->destroy();
	glossPipe = nil;
}



/*
 * Neo Rim pipes
 */

rw::gl3::Shader *neoRimShader;
rw::gl3::Shader *neoRimSkinShader;

static void
uploadRimData(bool enable)
{
	using namespace rw;
	using namespace rw::gl3;

	V3d viewVec = rw::engine->currentCamera->getFrame()->getLTM()->at;
	glUniform3fv(U(u_viewVec), 1, (float*)&viewVec);
	float rimData[4];
	rimData[0] = Offset.Get();
	rimData[1] = Scale.Get();
	if(enable)
		rimData[2] = Scaling.Get()*RimlightMult;
	else
		rimData[2] = 0.0f;
	rimData[3] = 0.0f;
	glUniform3fv(U(u_rimData), 1, rimData);
	Color col = RampStart.Get();
	glUniform4fv(U(u_rampStart), 1, (float*)&col);
	col = RampEnd.Get();
	glUniform4fv(U(u_rampEnd), 1, (float*)&col);
}

static void
rimSkinRenderCB(rw::Atomic *atomic, rw::gl3::InstanceDataHeader *header)
{
	using namespace rw;
	using namespace rw::gl3;

	if(!RimlightEnable){
		gl3::skinRenderCB(atomic, header);
		return;
	}

	Material *m;

	setWorldMatrix(atomic->getFrame()->getLTM());
	lightingCB(atomic);

#ifdef RW_GL_USE_VAOS
	glBindVertexArray(header->vao);
#else
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, header->ibo);
	glBindBuffer(GL_ARRAY_BUFFER, header->vbo);
	setAttribPointers(header->attribDesc, header->numAttribs);
#endif

	InstanceData *inst = header->inst;
	rw::int32 n = header->numMeshes;

	neoRimSkinShader->use();

	uploadRimData(atomic->geometry->flags & Geometry::LIGHT);

	uploadSkinMatrices(atomic);

	while(n--){
		m = inst->material;

		setMaterial(m->color, m->surfaceProps);

		setTexture(0, m->texture);

		rw::SetRenderState(VERTEXALPHA, inst->vertexAlpha || m->color.alpha != 0xFF);

		drawInst(header, inst);
		inst++;
	}
#ifndef RW_GL_USE_VAOS
	disableAttribPointers(header->attribDesc, header->numAttribs);
#endif
}

static void
rimRenderCB(rw::Atomic *atomic, rw::gl3::InstanceDataHeader *header)
{
	using namespace rw;
	using namespace rw::gl3;

	if(!RimlightEnable){
		gl3::defaultRenderCB(atomic, header);
		return;
	}

	Material *m;

	setWorldMatrix(atomic->getFrame()->getLTM());
	lightingCB(atomic);

#ifdef RW_GL_USE_VAOS
	glBindVertexArray(header->vao);
#else
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, header->ibo);
	glBindBuffer(GL_ARRAY_BUFFER, header->vbo);
	setAttribPointers(header->attribDesc, header->numAttribs);
#endif

	InstanceData *inst = header->inst;
	rw::int32 n = header->numMeshes;

	neoRimShader->use();

	uploadRimData(atomic->geometry->flags & Geometry::LIGHT);

	while(n--){
		m = inst->material;

		setMaterial(m->color, m->surfaceProps);

		setTexture(0, m->texture);

		rw::SetRenderState(VERTEXALPHA, inst->vertexAlpha || m->color.alpha != 0xFF);

		drawInst(header, inst);
		inst++;
	}
#ifndef RW_GL_USE_VAOS
	disableAttribPointers(header->attribDesc, header->numAttribs);
#endif
}

void
CreateRimLightPipes(void)
{
	using namespace rw::gl3;

	if(CFileMgr::LoadFile("neo/rimTweakingTable.dat", work_buff, sizeof(work_buff), "r") <= 0)
		printf("Error: couldn't open 'neo/rimTweakingTable.dat'\n");
	else{
		char *fp = (char*)work_buff;
		fp = ReadTweakValueTable(fp, RampStart);
		fp = ReadTweakValueTable(fp, RampEnd);
		fp = ReadTweakValueTable(fp, Offset);
		fp = ReadTweakValueTable(fp, Scale);
		fp = ReadTweakValueTable(fp, Scaling);
	}

	{
#include "shaders/simple_fs_gl.inc"
#include "shaders/neoRimSkin_gl.inc"
	const char *vs[] = { shaderDecl, header_vert_src, neoRimSkin_vert_src, nil };
	const char *fs[] = { shaderDecl, header_frag_src, simple_frag_src, nil };
	neoRimSkinShader = Shader::create(vs, fs);
	assert(neoRimSkinShader);
	}

	{
#include "shaders/simple_fs_gl.inc"
#include "shaders/neoRim_gl.inc"
	const char *vs[] = { shaderDecl, header_vert_src, neoRim_vert_src, nil };
	const char *fs[] = { shaderDecl, header_frag_src, simple_frag_src, nil };
	neoRimShader = Shader::create(vs, fs);
	assert(neoRimShader);
	}


	rw::gl3::ObjPipeline *pipe = rw::gl3::ObjPipeline::create();
	pipe->instanceCB = rw::gl3::defaultInstanceCB;
	pipe->uninstanceCB = nil;
	pipe->renderCB = rimRenderCB;
	rimPipe = pipe;

	pipe = rw::gl3::ObjPipeline::create();
	pipe->instanceCB = rw::gl3::skinInstanceCB;
	pipe->uninstanceCB = nil;
	pipe->renderCB = rimSkinRenderCB;
	rimSkinPipe = pipe;
}

void
DestroyRimLightPipes(void)
{
	neoRimShader->destroy();
	neoRimShader = nil;

	neoRimSkinShader->destroy();
	neoRimSkinShader = nil;

	((rw::gl3::ObjPipeline*)rimPipe)->destroy();
	rimPipe = nil;

	((rw::gl3::ObjPipeline*)rimSkinPipe)->destroy();
	rimSkinPipe = nil;
}



void
CustomPipeRegisterGL(void)
{
	u_viewVec = rw::gl3::registerUniform("u_viewVec");
	u_rampStart = rw::gl3::registerUniform("u_rampStart");
	u_rampEnd = rw::gl3::registerUniform("u_rampEnd");
	u_rimData = rw::gl3::registerUniform("u_rimData");

	u_lightMap = rw::gl3::registerUniform("u_lightMap");

	u_eye = rw::gl3::registerUniform("u_eye");
	u_reflProps = rw::gl3::registerUniform("u_reflProps");
	u_specDir = rw::gl3::registerUniform("u_specDir");
	u_specColor = rw::gl3::registerUniform("u_specColor");

	u_amb = rw::gl3::registerUniform("u_amb");
	u_emiss = rw::gl3::registerUniform("u_emiss");
	u_colorscale = rw::gl3::registerUniform("u_colorscale");

	u_texMatrix = rw::gl3::registerUniform("u_texMatrix");
	u_fxparams = rw::gl3::registerUniform("u_fxparams");
}


}

#ifdef NEW_RENDERER

namespace WorldRender
{

struct BuildingInst
{
	rw::Matrix matrix;
	rw::gl3::InstanceDataHeader *instHeader;
	uint8 fadeAlpha;
	bool lighting;
};
BuildingInst blendInsts[3][2000];
int numBlendInsts[3];

static RwRGBAReal black;

static bool
IsTextureTransparent(RwTexture *tex)
{
	if(tex == nil || tex->raster == nil)
		return false;
	return PLUGINOFFSET(rw::gl3::Gl3Raster, tex->raster, rw::gl3::nativeRasterOffset)->hasAlpha;
}

// Render all opaque meshes and put atomics that needs blending
// into the deferred list.
void
AtomicFirstPass(RpAtomic *atomic, int pass)
{
	using namespace rw;
	using namespace rw::gl3;

	BuildingInst *building = &blendInsts[pass][numBlendInsts[pass]];

	atomic->getPipeline()->instance(atomic);
	building->instHeader = (gl3::InstanceDataHeader*)atomic->geometry->instData;
	assert(building->instHeader != nil);
	assert(building->instHeader->platform == PLATFORM_GL3);
	building->fadeAlpha = 255;
	building->lighting = !!(atomic->geometry->flags & rw::Geometry::LIGHT);

	bool setupDone = false;
	bool defer = false;
	building->matrix = *atomic->getFrame()->getLTM();

	float colorscale[4];

	InstanceData *inst = building->instHeader->inst;
	for(rw::uint32 i = 0; i < building->instHeader->numMeshes; i++, inst++){
		Material *m = inst->material;

		if(m->texture == nil)
			continue;

		if(inst->vertexAlpha || m->color.alpha != 255 ||
		   IsTextureTransparent(m->texture)){
			defer = true;
			continue;
		}

		// alright we're rendering this atomic
		if(!setupDone){
			if(CustomPipes::WorldPipeSwitch == CustomPipes::WORLDPIPE_MOBILE)
				CustomPipes::leedsWorldShader_mobile->use();
			else
				CustomPipes::leedsWorldShader->use();
			setWorldMatrix(&building->matrix);
#ifdef RW_GL_USE_VAOS
			glBindVertexArray(building->instHeader->vao);
#else
			glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, building->instHeader->ibo);
			glBindBuffer(GL_ARRAY_BUFFER, building->instHeader->vbo);
			setAttribPointers(building->instHeader->attribDesc, building->instHeader->numAttribs);
#endif

			RGBAf amb, emiss;
			amb.red = CTimeCycle::GetAmbientRed();
			amb.green = CTimeCycle::GetAmbientGreen();
			amb.blue = CTimeCycle::GetAmbientBlue();
			amb.alpha = 1.0f;
			emiss = pAmbient->color;

			glUniform4fv(U(CustomPipes::u_amb), 1, (float*)&amb);
			glUniform4fv(U(CustomPipes::u_emiss), 1, (float*)&emiss);

			colorscale[3] = 1.0f;

			setupDone = true;
		}

		setMaterial(m->color, m->surfaceProps, 0.5f);

		float cs = 1.0f;
		if(CustomPipes::WorldPipeSwitch == CustomPipes::WORLDPIPE_PS2 && m->texture)
			cs = 255/128.0f;
		colorscale[0] = colorscale[1] = colorscale[2] = cs;
		glUniform4fv(U(CustomPipes::u_colorscale), 1, colorscale);

		setTexture(0, m->texture);

		drawInst(building->instHeader, inst);
	}
#ifndef RW_GL_USE_VAOS
	disableAttribPointers(building->instHeader->attribDesc, building->instHeader->numAttribs);
#endif
	if(defer)
		numBlendInsts[pass]++;
}

void
AtomicFullyTransparent(RpAtomic *atomic, int pass, int fadeAlpha)
{
	using namespace rw;
	using namespace rw::gl3;

	BuildingInst *building = &blendInsts[pass][numBlendInsts[pass]];

	atomic->getPipeline()->instance(atomic);
	building->instHeader = (gl3::InstanceDataHeader*)atomic->geometry->instData;
	assert(building->instHeader != nil);
	assert(building->instHeader->platform == PLATFORM_GL3);
	building->fadeAlpha = fadeAlpha;
	building->lighting = !!(atomic->geometry->flags & rw::Geometry::LIGHT);
	building->matrix = *atomic->getFrame()->getLTM();
	numBlendInsts[pass]++;
}

void
RenderBlendPass(int pass)
{
	using namespace rw;
	using namespace rw::gl3;

	if(CustomPipes::WorldPipeSwitch == CustomPipes::WORLDPIPE_MOBILE)
		CustomPipes::leedsWorldShader_mobile->use();
	else
		CustomPipes::leedsWorldShader->use();

	RGBAf amb, emiss;
	amb.red = CTimeCycle::GetAmbientRed();
	amb.green = CTimeCycle::GetAmbientGreen();
	amb.blue = CTimeCycle::GetAmbientBlue();
	amb.alpha = 1.0f;
	emiss = pAmbient->color;

	glUniform4fv(U(CustomPipes::u_amb), 1, (float*)&amb);
	glUniform4fv(U(CustomPipes::u_emiss), 1, (float*)&emiss);

	float colorscale[4];
	colorscale[3] = 1.0f;

	int i;
	for(i = 0; i < numBlendInsts[pass]; i++){
		BuildingInst *building = &blendInsts[pass][i];

#ifdef RW_GL_USE_VAOS
		glBindVertexArray(building->instHeader->vao);
#else
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, building->instHeader->ibo);
		glBindBuffer(GL_ARRAY_BUFFER, building->instHeader->vbo);
		setAttribPointers(building->instHeader->attribDesc, building->instHeader->numAttribs);
#endif
		setWorldMatrix(&building->matrix);

		InstanceData *inst = building->instHeader->inst;
		for(rw::uint32 j = 0; j < building->instHeader->numMeshes; j++, inst++){
			Material *m = inst->material;
			if(m->texture == nil)
				continue;
			if(!inst->vertexAlpha && m->color.alpha == 255 && !IsTextureTransparent(m->texture) && building->fadeAlpha == 255)
				continue;	// already done this one

			rw::RGBA color = m->color;
			color.alpha = (color.alpha * building->fadeAlpha)/255;
			setMaterial(color, m->surfaceProps, 0.5f);

			float cs = 1.0f;
			if(CustomPipes::WorldPipeSwitch == CustomPipes::WORLDPIPE_PS2 && m->texture)
				cs = 255/128.0f;
			colorscale[0] = colorscale[1] = colorscale[2] = cs;
			glUniform4fv(U(CustomPipes::u_colorscale), 1, colorscale);

			setTexture(0, m->texture);

			drawInst(building->instHeader, inst);
		}
#ifndef RW_GL_USE_VAOS
		disableAttribPointers(building->instHeader->attribDesc, building->instHeader->numAttribs);
#endif
	}
}
}
#endif

#endif
#endif
