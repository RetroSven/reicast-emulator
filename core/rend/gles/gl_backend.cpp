#include <math.h>
#include <map>

#include <memalign.h>

#include <algorithm>

#include <glsm/glsm.h>
#include <glsm/glsmsym.h>

#ifdef __SSE4_1__
#include <xmmintrin.h>
#endif

#include "gl_backend.h"
#include "../rend.h"
#include "../../libretro/libretro.h"

#include "../../hw/pvr/pvr.h"
#include "../../hw/mem/_vmem.h"
#include "../../hw/pvr/tr.h"
#include "../../hw/pvr/pixel_convert.h"

#define VERTEX_POS_ARRAY      0
#define VERTEX_COL_BASE_ARRAY 1
#define VERTEX_COL_OFFS_ARRAY 2
#define VERTEX_UV_ARRAY       3

extern retro_environment_t environ_cb;
extern bool fog_needs_update;
extern bool enable_rtt;
bool KillTex=false;

struct modvol_shader_type
{
   GLuint program;
   GLuint scale;
   GLuint depth_scale;
   GLuint sp_ShaderColor;
};

struct PipelineShader
{
	GLuint program;
	GLuint scale;
   GLuint depth_scale;
	GLuint pp_ClipTest;
   GLuint cp_AlphaTestValue;
	GLuint sp_FOG_COL_RAM;
   GLuint sp_FOG_COL_VERT;
   GLuint sp_FOG_DENSITY;
   GLuint sp_LOG_FOG_COEFS;
	u32 cp_AlphaTest;
   s32 pp_ClipTestMode;
	u32 pp_Texture;
   u32 pp_UseAlpha;
   u32 pp_IgnoreTexA;
   u32 pp_ShadInstr;
   u32 pp_Offset;
   u32 pp_FogCtrl;
};

struct ShaderUniforms_t
{
	float PT_ALPHA;
	float scale_coefs[4];
	float depth_coefs[4];
	float fog_den_float;
	float ps_FOG_COL_RAM[3];
	float ps_FOG_COL_VERT[3];
	float fog_coefs[2];
} ShaderUniforms;

struct vbo_type
{
   GLuint geometry;
   GLuint modvols;
   GLuint idxs;
   GLuint idxs2;
};

vbo_type vbo;
modvol_shader_type modvol_shader;
PipelineShader program_table[768*2];

/*

Drawing and related state management
Takes vertex, textures and renders to the currently set up target
*/

const static u32 CullMode[]= 
{

	GL_NONE, //0    No culling          No culling
	GL_NONE, //1    Cull if Small       Cull if ( |det| < fpu_cull_val )

	GL_FRONT, //2   Cull if Negative    Cull if ( |det| < 0 ) or ( |det| < fpu_cull_val )
	GL_BACK,  //3   Cull if Positive    Cull if ( |det| > 0 ) or ( |det| < fpu_cull_val )
};
const static u32 Zfunction[]=
{
	GL_NEVER,      //GL_NEVER,              //0 Never
	GL_LESS,        //GL_LESS/*EQUAL*/,     //1 Less
	GL_EQUAL,       //GL_EQUAL,             //2 Equal
	GL_LEQUAL,      //GL_LEQUAL,            //3 Less Or Equal
	GL_GREATER,     //GL_GREATER/*EQUAL*/,  //4 Greater
	GL_NOTEQUAL,    //GL_NOTEQUAL,          //5 Not Equal
	GL_GEQUAL,      //GL_GEQUAL,            //6 Greater Or Equal
	GL_ALWAYS,      //GL_ALWAYS,            //7 Always
};

/*
0   Zero                  (0, 0, 0, 0)
1   One                   (1, 1, 1, 1)
2   Dither Color          (OR, OG, OB, OA) 
3   Inverse Dither Color  (1-OR, 1-OG, 1-OB, 1-OA)
4   SRC Alpha             (SA, SA, SA, SA)
5   Inverse SRC Alpha     (1-SA, 1-SA, 1-SA, 1-SA)
6   DST Alpha             (DA, DA, DA, DA)
7   Inverse DST Alpha     (1-DA, 1-DA, 1-DA, 1-DA)
*/

const static u32 DstBlendGL[] =
{
	GL_ZERO,
	GL_ONE,
	GL_SRC_COLOR,
	GL_ONE_MINUS_SRC_COLOR,
	GL_SRC_ALPHA,
	GL_ONE_MINUS_SRC_ALPHA,
	GL_DST_ALPHA,
	GL_ONE_MINUS_DST_ALPHA
};

const static u32 SrcBlendGL[] =
{
	GL_ZERO,
	GL_ONE,
	GL_DST_COLOR,
	GL_ONE_MINUS_DST_COLOR,
	GL_SRC_ALPHA,
	GL_ONE_MINUS_SRC_ALPHA,
	GL_DST_ALPHA,
	GL_ONE_MINUS_DST_ALPHA
};

Vertex* vtx_sort_base;

struct IndexTrig
{
	u16 id[3];
	u16 pid;
	f32 z;
};

struct SortTrigDrawParam
{
	PolyParam* ppid;
	u16 first;
	u16 count;
};

static vector<SortTrigDrawParam>	pidx_sort;
PipelineShader* CurrentShader;
static u32 gcflip;

static struct
{
	TSP tsp;
	//TCW tcw;
	PCW pcw;
	ISP_TSP isp;
	u32 clipmode;
	//u32 texture_enabled;
	u32 stencil_modvol_on;
	u32 program;
	GLuint texture;

	void Reset(const PolyParam* gp)
	{
		program=~0;
		texture=~0;
		tsp.full = ~gp->tsp.full;
		//tcw.full = ~gp->tcw.full;
		pcw.full = ~gp->pcw.full;
		isp.full = ~gp->isp.full;
		clipmode=0xFFFFFFFF;
//		texture_enabled=~gp->pcw.Texture;
		stencil_modvol_on=false;
	}
} cache;

typedef void TexConvFP(PixelBuffer* pb,u8* p_in,u32 Width,u32 Height);

struct PvrTexInfo
{
	const char* name;
	int bpp;        //4/8 for pal. 16 for uv, argb
	GLuint type;
	TexConvFP *PL;
	TexConvFP *TW;
	TexConvFP *VQ;
};

PvrTexInfo format[8]=
{
	{"1555", 16,GL_UNSIGNED_SHORT_5_5_5_1, &tex1555_PL,&tex1555_TW,&tex1555_VQ},	//1555
	{"565", 16,GL_UNSIGNED_SHORT_5_6_5,    &tex565_PL,&tex565_TW,&tex565_VQ},		//565
	{"4444", 16,GL_UNSIGNED_SHORT_4_4_4_4, &tex4444_PL,&tex4444_TW,&tex4444_VQ},	//4444
	{"yuv", 16,GL_UNSIGNED_SHORT_5_6_5,    &texYUV422_PL,&texYUV422_TW,&texYUV422_VQ},	//yuv
	{"UNSUPPORTED BUMP MAPPED POLY", 16,GL_UNSIGNED_SHORT_4_4_4_4,&texBMP_PL,&texBMP_TW,&texBMP_VQ},	//bump_ns
	{"pal4", 4,0,0,texPAL4_TW,0},	//pal4
	{"pla8", 8,0,0,texPAL8_TW,0},	//pal8
	{"ns/1555", 0},	//ns, 1555
};

const u32 compressed_mipmap_offsets[8] =
{
	0x00006, /*    8  x 8*/
	0x00016, /*   16  x 16*/
	0x00056, /*   32  x 32 */
	0x00156, /*   64  x 64 */
	0x00556, /*  128  x 128*/
	0x01556, /*  256  x 256*/
	0x05556, /*  512  x 512 */
	0x15556  /* 1024  x 1024 */
};

const GLuint PAL_TYPE[4]=
{GL_UNSIGNED_SHORT_5_5_5_1,GL_UNSIGNED_SHORT_5_6_5,GL_UNSIGNED_SHORT_4_4_4_4,GL_UNSIGNED_SHORT_4_4_4_4};

u16 temp_tex_buffer[1024*1024];
extern u32 decoded_colors[3][65536];

struct FBT
{
	u32 TexAddr;
	GLuint depthb,stencilb;
	GLuint tex;
	GLuint fbo;
};

FBT fb_rtt;

/* Texture Cache */
struct TextureCacheData
{
	TSP tsp;             /* PowerVR texture parameters */
	TCW tcw;

   GLuint texID;        /* GL texture ID */
	u16* pData;
	int tex_type;

	u32 Lookups;

	/* decoded texture info */
   u32 sa;              /* pixel data start address in VRAM (might be offset for mipmaps/etc) */
   u32 sa_tex;		      /* texture data start address in VRAM */
   u32 w,h;             /* Width & height of the texture */
   u32 size;            /* Size, in bytes, in VRAM */

	PvrTexInfo *tex;
	TexConvFP  *texconv;

	u32 dirty;
	vram_block* lock_block;

	u32 Updates;

	/* Used for palette updates */
	u32  pal_local_rev;         /* Local palette rev */
	u32* pal_table_rev;         /* Table palette rev pointer */
	u32  indirect_color_ptr;    /* Palette color table index for paletted texture */

	                            /* VQ quantizers table for VQ texture.
	                             * A texture can't be both VQ and PAL (paletted) at the same time */

   void SetRepeatMode(GLuint dir,u32 clamp,u32 mirror)
	{
		if (clamp)
			glTexParameteri (GL_TEXTURE_2D, dir, GL_CLAMP_TO_EDGE);
		else 
			glTexParameteri (GL_TEXTURE_2D, dir, mirror?GL_MIRRORED_REPEAT : GL_REPEAT);
	}

	//Create GL texture from tsp/tcw
	void Create(bool isGL)
	{
      texID      = 0;

		if (isGL)
			glGenTextures(1, &texID);
		
		/* Reset state info */
		pData      = 0;
		tex_type   = 0;
		Lookups    = 0;
		Updates    = 0;
		dirty      = FrameCount;
		lock_block = 0;

		/* Decode info from TSP/TCW into the texture struct */
		tex        = &format[tcw.PixelFmt==7?0:tcw.PixelFmt];		/* texture format table entry */

		sa_tex     = (tcw.TexAddr<<3) & VRAM_MASK;               /* texture start address */
		sa         = sa_tex;						                     /* data texture start address (modified for MIPs, as needed) */
		w          = 8 << tsp.TexU;                              /* texture width */
		h          = 8 << tsp.TexV;                              /* texture height */

		if (texID)
      {
         /* Bind texture to set modes */
         glBindTexture(GL_TEXTURE_2D, texID);

         /* Set texture repeat mode */
         SetRepeatMode(GL_TEXTURE_WRAP_S, tsp.ClampU, tsp.FlipU); // glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (tsp.ClampU ? GL_CLAMP_TO_EDGE : (tsp.FlipU ? GL_MIRRORED_REPEAT : GL_REPEAT))) ;
			SetRepeatMode(GL_TEXTURE_WRAP_T, tsp.ClampV, tsp.FlipV); // glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (tsp.ClampV ? GL_CLAMP_TO_EDGE : (tsp.FlipV ? GL_MIRRORED_REPEAT : GL_REPEAT))) ;

#ifdef HAVE_OPENGLES
         glHint(GL_GENERATE_MIPMAP_HINT, GL_NICEST);
#endif

         /* Set texture filter mode */
         if (tsp.FilterMode == 0)
         {
            /* Disable filtering, mipmaps */
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
         }
         else
         {
            /* Bilinear filtering */
            /* PowerVR supports also trilinear via two passes, but we ignore that for now */
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,
                  (tcw.MipMapped && settings.rend.UseMipmaps)?GL_LINEAR_MIPMAP_NEAREST:GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
         }
      }

      pal_table_rev = 0;

		/* PAL texture */
      if (tex->bpp == 4)
      {
         pal_table_rev=&pal_rev_16[tcw.PalSelect];
         indirect_color_ptr=tcw.PalSelect<<4;
      }
      else if (tex->bpp == 8)
      {
         pal_table_rev=&pal_rev_256[tcw.PalSelect>>4];
         indirect_color_ptr=(tcw.PalSelect>>4)<<8;
      }
      else
      {
         pal_table_rev=0;
      }

		/* VQ table (if VQ texture) */
		if (tcw.VQ_Comp)
			indirect_color_ptr = sa;

      texconv = 0;

      /* Convert a PVR texture into OpenGL */
		switch (tcw.PixelFmt)
      {
         case TA_PIXEL_1555:     /* ARGB1555  - value: 1 bit; RGB values: 5 bits each */
         case TA_PIXEL_RESERVED: /* RESERVED1 - Regarded as 1555 */
         case TA_PIXEL_565:      /* RGB565    - R value: 5 bits; G value: 6 bits; B value: 5 bits */
         case TA_PIXEL_4444:     /* ARGB4444  - value: 4 bits; RGB values: 4 bits each */
         case TA_PIXEL_YUV422:   /* YUV422    - 32 bits per 2 pixels; YUYV values: 8 bits each */
         case TA_PIXEL_BUMPMAP:  /* BUMPMAP   - NOT_PROPERLY SUPPORTED- 16 bits/pixel; S value: 8 bits; R value: 8 bits */
         case TA_PIXEL_4BPP:     /* 4BPP      - Palette texture with 4 bits/pixel */
         case TA_PIXEL_8BPP:     /* 8BPP      - Palette texture with 8 bits/pixel */
            if (tcw.ScanOrder && tex->PL)
            {
               int stride = w;

               /* Planar textures support stride selection,
                * mostly used for NPOT textures (videos). */
               if (tcw.StrideSel)
                  stride  = (TEXT_CONTROL&31)*32;

               texconv    = tex->PL;                  /* Call the format specific conversion code */
               size       = stride * h * tex->bpp/8;  /* Calculate the size, in bytes, for the locking. */
            }
            else
            {
               size = w * h;
               if (tcw.VQ_Comp)
               {
                  indirect_color_ptr = sa;
                  if (tcw.MipMapped)
                     sa             += compressed_mipmap_offsets[tsp.TexU];
                  texconv            = tex->VQ;
               }
               else
               {
                  if (tcw.MipMapped)
                     sa             += compressed_mipmap_offsets[tsp.TexU]*tex->bpp/2;
                  texconv            = tex->TW;
                  size              *= tex->bpp;
               }
               size /= 8;
            }
            break;
         default:
            printf("Unhandled texture %d\n",tcw.PixelFmt);
            size=w*h*2;
            memset(temp_tex_buffer,0xFFFFFFFF,size);
            break;
      }
	}

	void Update(void)
   {
      GLuint textype;

      Updates++;                                   /* texture state tracking stuff */
      dirty              = 0;
      textype            = tex->type;

      if (pal_table_rev) 
      {
         textype         = PAL_TYPE[PAL_RAM_CTRL&3];
         pal_local_rev   = *pal_table_rev;             /* make sure to update the local rev, 
                                                      so it won't have to redo the texture */
      }

      palette_index      = indirect_color_ptr;              /* might be used if paletted texture */
      vq_codebook        = (u8*)&vram.data[indirect_color_ptr];  /* might be used if VQ texture */

      //texture conversion work
      PixelBuffer pbt;
      pbt.p_buffer_start = pbt.p_current_line=temp_tex_buffer;
      pbt.pixels_per_line=w;

      u32 stride         = w;

      if (tcw.StrideSel && tcw.ScanOrder && tex->PL) 
         stride = (TEXT_CONTROL&31)*32; //I think this needs +1 ?

      if(texconv)
         texconv(&pbt,(u8*)&vram.data[sa], stride, h);
      else
      {
         /* fill it in with a temporary color. */
         printf("UNHANDLED TEXTURE\n");
         memset(temp_tex_buffer,0xF88F8F7F,w*h*2);
      }

      /* lock the texture to detect changes in it. */
      lock_block = libCore_vramlock_Lock(sa_tex,sa+size-1,this);

      if (texID)
      {
         glBindTexture(GL_TEXTURE_2D, texID);
         GLuint comps=textype==GL_UNSIGNED_SHORT_5_6_5?GL_RGB:GL_RGBA;
         glTexImage2D(GL_TEXTURE_2D, 0,comps , w, h, 0, comps, textype, temp_tex_buffer);
         if (tcw.MipMapped && settings.rend.UseMipmaps)
            glGenerateMipmap(GL_TEXTURE_2D);
      }
      else
      {
#if FEAT_HAS_SOFTREND
         if (textype == GL_UNSIGNED_SHORT_5_6_5)
            tex_type = 0;
         else if (textype == GL_UNSIGNED_SHORT_5_5_5_1)
            tex_type = 1;
         else if (textype == GL_UNSIGNED_SHORT_4_4_4_4)
            tex_type = 2;

         if (pData)
         {
#ifdef __SSE4_1__
            _mm_free(pData);
#else
            memalign_free(pData);
#endif
         }

#ifdef __SSE4_1__
         pData = (u16*)_mm_malloc(w * h * 16, 16);
#else
         pData = (u16*)memalign_alloc(16, w * h * 16);
#endif
         for (int y = 0; y < h; y++)
         {
            for (int x = 0; x < w; x++)
            {
               u32* data = (u32*)&pData[(x + y*w) * 8];

               data[0]   = decoded_colors[tex_type][temp_tex_buffer[(x + 1) % w + (y + 1) % h * w]];
               data[1]   = decoded_colors[tex_type][temp_tex_buffer[(x + 0) % w + (y + 1) % h * w]];
               data[2]   = decoded_colors[tex_type][temp_tex_buffer[(x + 1) % w + (y + 0) % h * w]];
               data[3]   = decoded_colors[tex_type][temp_tex_buffer[(x + 0) % w + (y + 0) % h * w]];
            }
         }
#endif
      }
   }

	/* true if : dirty or paletted texture and revs don't match */
	bool NeedsUpdate()
   { 
      return (dirty) || (pal_table_rev!=0 && *pal_table_rev!=pal_local_rev);
   }
	
	void Delete()
	{
#if FEAT_HAS_SOFTREND
      if (pData)
#ifdef __SSE4_1__
         _mm_free(pData);
#else
         memalign_free(pData);
#endif
      pData = 0;
#endif

		if (texID)
         glDeleteTextures(1, &texID);
      texID = 0;
		if (lock_block)
			libCore_vramlock_Unlock_block(lock_block);
		lock_block=0;
	}
};

float fb_scale_x = 0.0f;
float fb_scale_y = 0.0f;

#define attr "attribute"
#define vary "varying"
#define FRAGCOL "gl_FragColor"
#define TEXLOOKUP "texture2D"

#ifdef HAVE_OPENGLES
#define HIGHP "highp"
#define MEDIUMP "mediump"
#define LOWP "lowp"
#else
#define HIGHP
#define MEDIUMP
#define LOWP
#endif

//Fragment and vertex shaders code
//pretty much 1:1 copy of the d3d ones for now
const char* VertexShaderSource =
#ifndef HAVE_OPENGLES
   "#version 120 \n"
#endif
"\
/* Vertex constants*/  \n\
uniform " HIGHP " vec4      scale; \n\
uniform " HIGHP " vec4      depth_scale; \n\
/* Vertex input */ \n\
" attr " " HIGHP " vec4    in_pos; \n\
" attr " " LOWP " vec4     in_base; \n\
" attr " " LOWP " vec4     in_offs; \n\
" attr " " MEDIUMP " vec2  in_uv; \n\
/* output */ \n\
" vary " " LOWP " vec4 vtx_base; \n\
" vary " " LOWP " vec4 vtx_offs; \n\
" vary " " MEDIUMP " vec2 vtx_uv; \n\
void main() \n\
{ \n\
	vtx_base=in_base; \n\
	vtx_offs=in_offs; \n\
	vtx_uv=in_uv; \n\
	vec4 vpos=in_pos; \n\
	vpos.w=1.0/vpos.z;  \n"
#ifndef GLES
	"\
   if (vpos.w < 0.0) { \n\
      gl_Position = vec4(0.0, 0.0, 0.0, vpos.w); \n\
         return; \n\
   } \n\
   vpos.z = vpos.w; \n"
#else
	"\
   vpos.z=depth_scale.x+depth_scale.y*vpos.w;  \n"
#endif
   "\
	vpos.xy=vpos.xy*scale.xy-scale.zw;  \n\
	vpos.xy*=vpos.w;  \n\
	gl_Position = vpos; \n\
}";

const char* PixelPipelineShader =
#ifndef HAVE_OPENGLES
      "#version 120 \n"
#endif
"\
\
#define cp_AlphaTest %d \n\
#define pp_ClipTestMode %d \n\
#define pp_UseAlpha %d \n\
#define pp_Texture %d \n\
#define pp_IgnoreTexA %d \n\
#define pp_ShadInstr %d \n\
#define pp_Offset %d \n\
#define pp_FogCtrl %d \n\
/* Shader program params*/ \n\
/* gles has no alpha test stage, so its emulated on the shader */ \n\
uniform " LOWP " float cp_AlphaTestValue; \n\
uniform " LOWP " vec4 pp_ClipTest; \n\
uniform " LOWP " vec3 sp_FOG_COL_RAM,sp_FOG_COL_VERT; \n\
uniform " HIGHP " vec2 sp_LOG_FOG_COEFS; \n\
uniform " HIGHP " float sp_FOG_DENSITY; \n\
uniform sampler2D tex,fog_table; \n\
/* Vertex input*/ \n\
" vary " " LOWP " vec4 vtx_base; \n\
" vary " " LOWP " vec4 vtx_offs; \n\
" vary " " MEDIUMP " vec2 vtx_uv; \n\
" LOWP " float fog_mode2(" HIGHP " float w) \n\
{ \n\
   " HIGHP " float fog_idx=clamp(w * sp_FOG_DENSITY, 0.0, 127.99); \n\
	return clamp(sp_LOG_FOG_COEFS.y * log2(fog_idx) + sp_LOG_FOG_COEFS.x, 0.001, 1.0); //the clamp is required due to yet another bug !\n\
} \n\
void main() \n\
{ \n\
	// Clip outside the box \n\
	#if pp_ClipTestMode==1 \n\
		if (gl_FragCoord.x < pp_ClipTest.x || gl_FragCoord.x > pp_ClipTest.z \n\
				|| gl_FragCoord.y < pp_ClipTest.y || gl_FragCoord.y > pp_ClipTest.w) \n\
			discard; \n\
	#endif \n\
	// Clip inside the box \n\
	#if pp_ClipTestMode==-1 \n\
		if (gl_FragCoord.x >= pp_ClipTest.x && gl_FragCoord.x <= pp_ClipTest.z \n\
				&& gl_FragCoord.y >= pp_ClipTest.y && gl_FragCoord.y <= pp_ClipTest.w) \n\
			discard; \n\
	#endif \n\
	\n\
   " LOWP " vec4 color=vtx_base; \n\
	#if pp_UseAlpha==0 \n\
		color.a=1.0; \n\
	#endif\n\
	#if pp_FogCtrl==3 \n\
		color=vec4(sp_FOG_COL_RAM.rgb,fog_mode2(gl_FragCoord.w)); \n\
	#endif\n\
	#if pp_Texture==1 \n\
	{ \n\
      " LOWP " vec4 texcol=" TEXLOOKUP "(tex,vtx_uv); \n\
		\n\
		#if pp_IgnoreTexA==1 \n\
			texcol.a=1.0;	 \n\
		#endif\n\
		\n\
      #if cp_AlphaTest == 1 \n\
         if (cp_AlphaTestValue>texcol.a) discard;\n\
      #endif \n\
		#if pp_ShadInstr==0 \n\
		{ \n\
         color=texcol; \n\
		} \n\
		#endif\n\
		#if pp_ShadInstr==1 \n\
		{ \n\
			color.rgb*=texcol.rgb; \n\
			color.a=texcol.a; \n\
		} \n\
		#endif\n\
		#if pp_ShadInstr==2 \n\
		{ \n\
			color.rgb=mix(color.rgb,texcol.rgb,texcol.a); \n\
		} \n\
		#endif\n\
		#if  pp_ShadInstr==3 \n\
		{ \n\
			color*=texcol; \n\
		} \n\
		#endif\n\
		\n\
		#if pp_Offset==1 \n\
		{ \n\
			color.rgb+=vtx_offs.rgb; \n\
			if (pp_FogCtrl==1) \n\
				color.rgb=mix(color.rgb,sp_FOG_COL_VERT.rgb,vtx_offs.a); \n\
		} \n\
		#endif\n\
	} \n\
	#endif\n\
	#if pp_FogCtrl==0 \n\
	{ \n\
		color.rgb=mix(color.rgb,sp_FOG_COL_RAM.rgb,fog_mode2(gl_FragCoord.w));  \n\
	} \n\
	#endif\n\
	#if cp_AlphaTest == 1 \n\
      color.a=1.0; \n\
	#endif  \n"
#ifndef GLES
   "\
	float w = gl_FragCoord.w * 100000.0; \n\
	gl_FragDepth = log2(1.0 + w) / 34; \n"
#endif
	FRAGCOL "=color; \n\
}";

const char* ModifierVolumeShader =
" \
uniform " LOWP " float sp_ShaderColor; \n\
/* Vertex input*/ \n\
void main() \n\
{ \n"
#ifndef GLES
	"\
	float w = gl_FragCoord.w * 100000.0; \n\
	gl_FragDepth = log2(1.0 + w) / 34; \n"
#endif
	FRAGCOL "=vec4(0.0, 0.0, 0.0, sp_ShaderColor); \n\
}";


static int gles_screen_width  = 640;
static int gles_screen_height = 480;

static s32 SetTileClip(u32 val, bool set)
{
	float csx=0,csy=0,cex=0,cey=0;
	u32 clipmode=val>>28;
	s32 clip_mode;
	if (clipmode<2)
		clip_mode=0;    //always passes
	else if (clipmode&1)
		clip_mode=-1;   //render stuff outside the region
	else
		clip_mode=1;    //render stuff inside the region

	csx=(float)(val&63);
	cex=(float)((val>>6)&63);
	csy=(float)((val>>12)&31);
	cey=(float)((val>>17)&31);
	csx=csx*32;
	cex=cex*32 +32;
	csy=csy*32;
	cey=cey*32 +32;

	if (csx <= 0 && csy <= 0 && cex >= 640 && cey >= 480)
		return 0;
	
	if (set && clip_mode)
   {
    	csy = 480 - csy;
		cey = 480 - cey;
		float dc2s_scale_h = gles_screen_height / 480.0f;
		float ds2s_offs_x = (gles_screen_width - dc2s_scale_h * 640) / 2;
		csx = csx * dc2s_scale_h + ds2s_offs_x;
		cex = cex * dc2s_scale_h + ds2s_offs_x;
		csy = csy * dc2s_scale_h;
		cey = cey * dc2s_scale_h;
		glUniform4f(CurrentShader->pp_ClipTest, csx, cey, cex, csy);		
   }

	return clip_mode;
}

static void SetCull(u32 CulliMode)
{
	if (CullMode[CulliMode] == GL_NONE)
		glDisable(GL_CULL_FACE);
	else
	{
		glEnable(GL_CULL_FACE);
		glCullFace(CullMode[CulliMode]); //GL_FRONT/GL_BACK, ...
	}
}

static int GetProgramID(
      u32 cp_AlphaTest,
      u32 pp_ClipTestMode,
      u32 pp_Texture,
      u32 pp_UseAlpha,
      u32 pp_IgnoreTexA,
      u32 pp_ShadInstr,
      u32 pp_Offset,
      u32 pp_FogCtrl)
{
	u32 rv=0;

	rv|=pp_ClipTestMode;
	rv<<=1; rv|=cp_AlphaTest;
	rv<<=1; rv|=pp_Texture;
	rv<<=1; rv|=pp_UseAlpha;
	rv<<=1; rv|=pp_IgnoreTexA;
	rv<<=2; rv|=pp_ShadInstr;
	rv<<=1; rv|=pp_Offset;
	rv<<=2; rv|=pp_FogCtrl;

	return rv;
}

static GLuint gl_CompileShader(const char* shader,GLuint type)
{
	GLint result;
	GLint compile_log_len;
	GLuint rv=glCreateShader(type);
	glShaderSource(rv, 1,&shader, NULL);
	glCompileShader(rv);

	//lets see if it compiled ...
	glGetShaderiv(rv, GL_COMPILE_STATUS, &result);
	glGetShaderiv(rv, GL_INFO_LOG_LENGTH, &compile_log_len);

	if (!result && compile_log_len>0)
	{
		if (compile_log_len==0)
			compile_log_len=1;
		char* compile_log=(char*)malloc(compile_log_len);
		*compile_log=0;

		glGetShaderInfoLog(rv, compile_log_len, &compile_log_len, compile_log);
		printf("Shader: %s \n%s\n",result?"compiled!":"failed to compile",compile_log);

		free(compile_log);
	}

	return rv;
}

static GLuint gl_CompileAndLink(const char* VertexShader, const char* FragmentShader)
{
	GLint compile_log_len;
	GLint result;
	/* Create vertex/fragment shaders */
	GLuint vs      = gl_CompileShader(VertexShader ,GL_VERTEX_SHADER);
	GLuint ps      = gl_CompileShader(FragmentShader ,GL_FRAGMENT_SHADER);
	GLuint program = glCreateProgram();

	glAttachShader(program, vs);
	glAttachShader(program, ps);

	/* Bind vertex attribute to VBO inputs */
	glBindAttribLocation(program, VERTEX_POS_ARRAY,      "in_pos");
	glBindAttribLocation(program, VERTEX_COL_BASE_ARRAY, "in_base");
	glBindAttribLocation(program, VERTEX_COL_OFFS_ARRAY, "in_offs");
	glBindAttribLocation(program, VERTEX_UV_ARRAY,       "in_uv");

#ifndef HAVE_OPENGLES
	glBindFragDataLocation(program, 0, "FragColor");
#endif

	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, &result);
	glGetProgramiv(program, GL_INFO_LOG_LENGTH, &compile_log_len);

	if (!result && compile_log_len>0)
	{
      char *compile_log = NULL;

		if (compile_log_len==0)
			compile_log_len = 1;
		compile_log_len   += 1024;
		compile_log        = (char*)malloc(compile_log_len);
		*compile_log       = 0;

		glGetProgramInfoLog(program, compile_log_len, &compile_log_len, compile_log);
		printf("Shader linking: %s \n (%d bytes), - %s -\n",result?"linked":"failed to link", compile_log_len,compile_log);

		free(compile_log);
		die("shader compile fail\n");
	}

	glDeleteShader(vs);
	glDeleteShader(ps);

	glUseProgram(program);

	verify(glIsProgram(program));

	return program;
}

static void set_shader_uniforms(struct ShaderUniforms_t *uni, PipelineShader* s)
{
   if (s->cp_AlphaTestValue!=-1)
      glUniform1f(s->cp_AlphaTestValue, uni->PT_ALPHA);

   if (s->scale!=-1)
      glUniform4fv( s->scale, 1, uni->scale_coefs);

   if (s->depth_scale!=-1)
      glUniform4fv( s->depth_scale, 1, uni->depth_coefs);

   if (s->sp_FOG_DENSITY!=-1)
      glUniform1f( s->sp_FOG_DENSITY, uni->fog_den_float);

   if (s->sp_FOG_COL_RAM!=-1)
      glUniform3fv( s->sp_FOG_COL_RAM, 1, uni->ps_FOG_COL_RAM);

   if (s->sp_FOG_COL_VERT!=-1)
      glUniform3fv( s->sp_FOG_COL_VERT, 1, uni->ps_FOG_COL_VERT);

   if (s->sp_LOG_FOG_COEFS!=-1)
      glUniform2fv(s->sp_LOG_FOG_COEFS,1, uni->fog_coefs);
}

static bool CompilePipelineShader(void *data)
{
	char pshader[8192];
   PipelineShader *s = (PipelineShader*)data;

	sprintf(pshader,PixelPipelineShader,
                s->cp_AlphaTest,s->pp_ClipTestMode,s->pp_UseAlpha,
                s->pp_Texture,s->pp_IgnoreTexA,s->pp_ShadInstr,s->pp_Offset,s->pp_FogCtrl);

	s->program            = gl_CompileAndLink(VertexShaderSource,pshader);


	//setup texture 0 as the input for the shader
	GLuint gu=glGetUniformLocation(s->program, "tex");
	if (s->pp_Texture==1)
		glUniform1i(gu,0);

	//get the uniform locations
	s->scale	             = glGetUniformLocation(s->program, "scale");
	s->depth_scale        = glGetUniformLocation(s->program, "depth_scale");


	s->pp_ClipTest        = glGetUniformLocation(s->program, "pp_ClipTest");

	s->sp_FOG_DENSITY     = glGetUniformLocation(s->program, "sp_FOG_DENSITY");

	s->cp_AlphaTestValue  = glGetUniformLocation(s->program, "cp_AlphaTestValue");

	//FOG_COL_RAM,FOG_COL_VERT,FOG_DENSITY;
	if (s->pp_FogCtrl==1 && s->pp_Texture==1)
		s->sp_FOG_COL_VERT = glGetUniformLocation(s->program, "sp_FOG_COL_VERT");
	else
		s->sp_FOG_COL_VERT = -1;
	if (s->pp_FogCtrl==0 || s->pp_FogCtrl==3)
	{
		s->sp_FOG_COL_RAM=glGetUniformLocation(s->program, "sp_FOG_COL_RAM");
		s->sp_LOG_FOG_COEFS=glGetUniformLocation(s->program, "sp_LOG_FOG_COEFS");
	}
	else
	{
		s->sp_FOG_COL_RAM=-1;
		s->sp_LOG_FOG_COEFS=-1;
	}

	set_shader_uniforms(&ShaderUniforms, s);

	return glIsProgram(s->program)==GL_TRUE;
}

template <u32 Type, bool SortingEnabled>
static __forceinline void SetGPState(const PolyParam* gp, u32 cflip)
{
   //force everything to be shadowed
   const u32 stencil = (gp->pcw.Shadow!=0)?0x80:0;

   /* Has to preserve cache_TSP/ISP
    * Can freely use cache TCW */

   int prog_id   = GetProgramID(
         (Type == TA_LIST_PUNCH_THROUGH) ? 1 : 0,
         SetTileClip(gp->tileclip,false)+1,
         gp->pcw.Texture,
         gp->tsp.UseAlpha,
         gp->tsp.IgnoreTexA,
         gp->tsp.ShadInstr,
         gp->pcw.Offset,
         gp->tsp.FogCtrl);
   CurrentShader = &program_table[prog_id];

   if (CurrentShader->program == -1)
      CompilePipelineShader(CurrentShader);

   if (CurrentShader->program != cache.program)
   {
      cache.program    = CurrentShader->program;
      glUseProgram(CurrentShader->program);
   }

   SetTileClip(gp->tileclip,true);

   if (cache.stencil_modvol_on!=stencil)
   {
      cache.stencil_modvol_on   = stencil;
      glStencilFunc(GL_ALWAYS, stencil, stencil);
   }

   bool texture_changed = false;

   if (gp->texid != cache.texture)
   {
      cache.texture=gp->texid;
      if (gp->texid != -1)
      {
         glBindTexture(GL_TEXTURE_2D, gp->texid);
         texture_changed = true;
      }
   }

   if (gp->tsp.full!=cache.tsp.full || texture_changed)
   {
      cache.tsp=gp->tsp;

      if (Type==TA_LIST_TRANSLUCENT)
      {
         glBlendFunc(SrcBlendGL[gp->tsp.SrcInstr], DstBlendGL[gp->tsp.DstInstr]);
      }
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (gp->tsp.ClampU ? GL_CLAMP_TO_EDGE : (gp->tsp.FlipU ? GL_MIRRORED_REPEAT : GL_REPEAT))) ;
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (gp->tsp.ClampV ? GL_CLAMP_TO_EDGE : (gp->tsp.FlipV ? GL_MIRRORED_REPEAT : GL_REPEAT))) ;

      //set texture filter mode
      if (gp->tsp.FilterMode == 0)
      {
         //disable filtering, mipmaps
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      }
      else
      {
         //bilinear filtering
			//PowerVR supports also trilinear via two passes, but we ignore that for now
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (gp->tcw.MipMapped && settings.rend.UseMipmaps) ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR);
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      }
   }

   //set cull mode !
   //cflip is required when exploding triangles for triangle sorting
   //gcflip is global clip flip, needed for when rendering to texture due to mirrored Y direction
   SetCull(gp->isp.CullMode ^ cflip ^ gcflip);


   if (gp->isp.full!= cache.isp.full)
   {
      GLboolean flag;
      cache.isp.full = gp->isp.full;

      /* Set Z mode, only if required */
      if (!(Type == TA_LIST_PUNCH_THROUGH || (Type == TA_LIST_TRANSLUCENT && SortingEnabled)))
         glDepthFunc(Zfunction[gp->isp.DepthMode]);

      flag    = !gp->isp.ZWriteDis;
      if (SortingEnabled && settings.pvr.Emulation.AlphaSortMode == 0)
         flag = GL_FALSE;
      glDepthMask(flag);
   }
}

template <u32 Type, bool SortingEnabled>
static void DrawList(const List<PolyParam>& gply)
{
   PolyParam* params=gply.head();
   int count=gply.used();

   /* We want at least 1 PParam */
   if (count==0)
      return;

   /* reset the cache state */
   cache.Reset(params);

   /* set some 'global' modes for all primitives */

   /* Z funct. can be fixed on these combinations, avoid setting it all the time */
   if (Type == TA_LIST_PUNCH_THROUGH || (Type == TA_LIST_TRANSLUCENT && SortingEnabled))
      glDepthFunc(Zfunction[6]);

   glEnable(GL_STENCIL_TEST);
   glStencilFunc(
         GL_ALWAYS,
         0,
         0);
   glStencilOp(GL_KEEP,
         GL_KEEP,
         GL_REPLACE);

   while(count-->0)
   {
      if (params->count>2) /* this actually happens for some games. No idea why .. */
      {
         SetGPState<Type,SortingEnabled>(params, 0);
         glDrawElements(GL_TRIANGLE_STRIP, params->count, GL_UNSIGNED_SHORT, (GLvoid*)(2*params->first));
      }

      params++;
   }

   glDisable(GL_STENCIL_TEST);
   glStencilFunc(
         GL_ALWAYS,
         0,
         1);
   glStencilOp(GL_KEEP,
         GL_KEEP,
         GL_KEEP);
}

bool operator<(const PolyParam &left, const PolyParam &right)
{
   /* put any condition you want to sort on here */
	return left.zvZ  < right.zvZ;
#if 0
	return left.zMin < right.zMax;
#endif
}

//Sort based on min-z of each strip
static void SortPParams(void)
{
   u16 *idx_base      = NULL;
   Vertex *vtx_base   = NULL;
   PolyParam *pp      = NULL;
   PolyParam *pp_end  = NULL;

   if (pvrrc.verts.used()==0 || pvrrc.global_param_tr.used()<=1)
      return;

   vtx_base          = pvrrc.verts.head();
   idx_base          = pvrrc.idx.head();
   pp                = pvrrc.global_param_tr.head();
   pp_end            = pp + pvrrc.global_param_tr.used();

   while(pp!=pp_end)
   {
      if (pp->count<2)
         pp->zvZ=0;
      else
      {
         u16*      idx   = idx_base+pp->first;
         Vertex*   vtx   = vtx_base+idx[0];
         Vertex* vtx_end = vtx_base + idx[pp->count-1]+1;
         u32 zv          = 0xFFFFFFFF;

         while(vtx!=vtx_end)
         {
            zv = min(zv,(u32&)vtx->z);
            vtx++;
         }

         pp->zvZ=(f32&)zv;
      }
      pp++;
   }

   std::stable_sort(pvrrc.global_param_tr.head(),pvrrc.global_param_tr.head()+pvrrc.global_param_tr.used());
}

static inline float min3(float v0,float v1,float v2)
{
	return min(min(v0,v1),v2);
}

static inline float max3(float v0,float v1,float v2)
{
	return max(max(v0,v1),v2);
}

static inline float minZ(Vertex* v,u16* mod)
{
	return min(min(v[mod[0]].z,v[mod[1]].z),v[mod[2]].z);
}

bool operator<(const IndexTrig &left, const IndexTrig &right)
{
	return left.z<right.z;
}

//are two poly params the same?
static inline bool PP_EQ(PolyParam* pp0, PolyParam* pp1)
{
   return 
      (pp0->pcw.full&PCW_DRAW_MASK)==(pp1->pcw.full&PCW_DRAW_MASK) 
      && pp0->isp.full==pp1->isp.full 
      && pp0->tcw.full==pp1->tcw.full
      && pp0->tsp.full==pp1->tsp.full
      && pp0->tileclip==pp1->tileclip;
}

static void GenSorted(void)
{
   static vector<IndexTrig> lst;
   static vector<u16> vidx_sort;

   static u32 vtx_cnt;
   int idx            = -1;
   int pfsti          =  0;

   pidx_sort.clear();

   if (pvrrc.verts.used()==0 || pvrrc.global_param_tr.used()<=1)
      return;

   Vertex* vtx_base=pvrrc.verts.head();
   u16* idx_base=pvrrc.idx.head();

   PolyParam* pp_base=pvrrc.global_param_tr.head();
   PolyParam* pp=pp_base;
   PolyParam* pp_end= pp + pvrrc.global_param_tr.used();

   Vertex* vtx_arr=vtx_base+idx_base[pp->first];
   vtx_sort_base=vtx_base;
   int vtx_count=idx_base[pp_end[-1].first+pp_end[-1].count-1]-idx_base[pp->first];
   if (vtx_count>vtx_cnt)
      vtx_cnt=vtx_count;

#if PRINT_SORT_STATS
   printf("TVTX: %d || %d\n",vtx_cnt,vtx_count);
#endif

   if (vtx_count<=0)
      return;

   /* Make lists of all triangles, with their PID and VID */

   lst.resize(vtx_count*4);

   while(pp != pp_end)
   {
      Vertex *vtx     = NULL;
      Vertex *vtx_end = NULL;
      u16 *idx        = NULL;
      u32 flip        = 0;
      u32 ppid        = (pp-pp_base);

      if (pp->count <= 2)
      {
         pp++;
         continue;
      }

      idx             = idx_base + pp->first;
      vtx             = vtx_base+idx[0];
      vtx_end         = vtx_base + idx[pp->count-1]-1;

      while(vtx != vtx_end)
      {
         Vertex *v0   = &vtx[0];
         Vertex *v1   = &vtx[1];
         Vertex *v2   = &vtx[2];

         if (flip)
         {
            v0=&vtx[2];
            v1=&vtx[1];
            v2=&vtx[0];
         }

         u16* d =lst[pfsti].id;
         Vertex *vb = vtx_base;
         d[0]=v0-vb;
         d[1]=v1-vb;
         d[2]=v2-vb;

         lst[pfsti].pid= ppid ;
         lst[pfsti].z = minZ(vtx_base,lst[pfsti].id);
         pfsti++;

         flip ^= 1;
         vtx++;
      }
      pp++;
   }

   u32 aused=pfsti;

   lst.resize(aused);

   /* sort them */
   std::stable_sort(lst.begin(),lst.end());

   /* Merge PIDs/draw commands if two different PIDs are actually equal */

   for (u32 k = 1; k < aused; k++)
   {
      if (lst[k].pid == lst[k-1].pid)
         continue;

      if (PP_EQ(&pp_base[lst[k].pid],&pp_base[lst[k-1].pid]))
         lst[k].pid=lst[k-1].pid;
   }

   /* Reassemble vertex indices into drawing commands */

   vidx_sort.resize(aused*3);

   for (u32 i=0; i<aused; i++)
   {
      SortTrigDrawParam stdp;
      int   pid          = lst[i].pid;
      u16* midx          = lst[i].id;

      vidx_sort[i*3 + 0] = midx[0];
      vidx_sort[i*3 + 1] = midx[1];
      vidx_sort[i*3 + 2] = midx[2];

      if (idx == pid)
         continue;

      stdp.ppid  = pp_base + pid;
      stdp.first = (u16)(i*3);
      stdp.count = 0;

      if (idx!=-1)
      {
         SortTrigDrawParam *last = &pidx_sort[pidx_sort.size()-1];

         if (last)
            last->count=stdp.first-last->first;
      }

      pidx_sort.push_back(stdp);
      idx=pid;
   }

   SortTrigDrawParam *stdp = &pidx_sort[pidx_sort.size()-1];

   if (stdp)
      stdp->count=aused*3-stdp->first;

#if PRINT_SORT_STATS
   printf("Reassembled into %d from %d\n",pidx_sort.size(),pp_end-pp_base);
#endif

   /* Upload to GPU if needed, otherwise return */
   if (!pidx_sort.size())
      return;

   /* Bind and upload sorted index buffer */
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vbo.idxs2);
   glBufferData(GL_ELEMENT_ARRAY_BUFFER,vidx_sort.size()*2,&vidx_sort[0],GL_STREAM_DRAW);
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

static void DrawSorted(u32 count)
{
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vbo.idxs2);

   cache.Reset(pidx_sort[0].ppid);

   //set some 'global' modes for all primitives

   //Z sorting is fixed for .. sorted stuff
   glDepthFunc(Zfunction[6]);

   glEnable(GL_STENCIL_TEST);
   glStencilFunc(GL_ALWAYS, 0, 0);
   glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

   for (u32 p=0; p<count; p++)
   {
      PolyParam* params = pidx_sort[p].ppid;
      if (pidx_sort[p].count>2) //this actually happens for some games. No idea why ..
      {
         SetGPState<TA_LIST_TRANSLUCENT, true>(params, 0);
         glDrawElements(GL_TRIANGLES, pidx_sort[p].count, GL_UNSIGNED_SHORT, (GLvoid*)(2*pidx_sort[p].first));
      }
      params++;
   }
}

//All pixels are in area 0 by default.
//If inside an 'in' volume, they are in area 1
//if inside an 'out' volume, they are in area 0
/*
	Stencil bits:
		bit 7: mv affected (must be preserved)
		bit 1: current volume state
		but 0: summary result (starts off as 0)

	Lower 2 bits:

	IN volume (logical OR):
	00 -> 00
	01 -> 01
	10 -> 01
	11 -> 01

	Out volume (logical AND):
	00 -> 00
	01 -> 00
	10 -> 00
	11 -> 01
*/
static void SetMVS_Mode(u32 mv_mode,ISP_Modvol ispc)
{
	if (mv_mode==0)	//normal trigs
	{
		//set states
		glEnable(GL_DEPTH_TEST);

		//write only bit 1
      glStencilMask(2);

      //no stencil testing
      glStencilFunc(GL_ALWAYS, 0, 2);

		//count the number of pixels in front of the Z buffer (and only keep the lower bit of the count)
      glStencilOp(GL_KEEP, GL_KEEP, GL_INVERT);
		//Cull mode needs to be set
		SetCull(ispc.CullMode);
	}
	else
	{
		//1 (last in) or 2 (last out)
		//each triangle forms the last of a volume

		//common states

		//no depth test
		glDisable(GL_DEPTH_TEST);

		//write bits 1:0
      glStencilMask(3);

		if (mv_mode==1)
		{
         // Inclusion volume
			//res : old : final 
			//0   : 0      : 00
			//0   : 1      : 01
			//1   : 0      : 01
			//1   : 1      : 01
			

			//if (1<=st) st=1; else st=0;
         glStencilFunc(GL_LEQUAL, 1, 3);

         glStencilOp(GL_ZERO, GL_ZERO, GL_REPLACE);

			/*
			//if !=0 -> set to 10
			verifyc(dev->SetRenderState(D3DRS_STENCILFUNC,D3DCMP_LESSEQUAL));
			verifyc(dev->SetRenderState(D3DRS_STENCILREF,1));					
			verifyc(dev->SetRenderState(D3DRS_STENCILPASS,D3DSTENCILOP_REPLACE));
			verifyc(dev->SetRenderState(D3DRS_STENCILFAIL,D3DSTENCILOP_ZERO));
			*/
		}
		else
		{
         // Exclusion volume
			/*
				I've only seen a single game use it, so i guess it doesn't matter ? (Zombie revenge)
				(actually, i think there was also another, racing game)
			*/

         // The initial value for exclusion volumes is 1 so we need to invert the result before and'ing.
			//res : old : final 
			//0   : 0   : 00
			//0   : 1   : 01
			//1   : 0   : 00
			//1   : 1   : 00

			//if (1 == st) st = 1; else st = 0;
         glStencilFunc(GL_EQUAL, 1, 3);
         glStencilOp(GL_ZERO, GL_ZERO, GL_REPLACE);
		}
	}
}

static void SetupMainVBO(void)
{
	glBindBuffer(GL_ARRAY_BUFFER, vbo.geometry);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vbo.idxs);

	//setup vertex buffers attrib pointers
	glEnableVertexAttribArray(VERTEX_POS_ARRAY);
	glVertexAttribPointer(VERTEX_POS_ARRAY, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex,x));

	glEnableVertexAttribArray(VERTEX_COL_BASE_ARRAY);
	glVertexAttribPointer(VERTEX_COL_BASE_ARRAY, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)offsetof(Vertex,col));

	glEnableVertexAttribArray(VERTEX_COL_OFFS_ARRAY);
	glVertexAttribPointer(VERTEX_COL_OFFS_ARRAY, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)offsetof(Vertex,vtx_spc));

	glEnableVertexAttribArray(VERTEX_UV_ARRAY);
	glVertexAttribPointer(VERTEX_UV_ARRAY, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex,u));

}

static void DrawModVols(void)
{
   /* A bit of explanation:
     * In theory it works like this: generate a 1-bit stencil for each polygon
     * volume, and then AND or OR it against the overall 1-bit tile stencil at 
     * the end of the volume. */

	if (pvrrc.modtrig.used()==0 || settings.pvr.Emulation.ModVolMode == 0)
		return;

	glBindBuffer(GL_ARRAY_BUFFER, vbo.modvols);

	//setup vertex buffers attrib pointers
	glEnableVertexAttribArray(VERTEX_POS_ARRAY);
	glVertexAttribPointer(VERTEX_POS_ARRAY, 3, GL_FLOAT, GL_FALSE, sizeof(float)*3, (void*)0);

	glDisableVertexAttribArray(VERTEX_UV_ARRAY);
	glDisableVertexAttribArray(VERTEX_COL_OFFS_ARRAY);
	glDisableVertexAttribArray(VERTEX_COL_BASE_ARRAY);

	glEnable(GL_BLEND);
   glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glUseProgram(modvol_shader.program);
	glUniform1f(modvol_shader.sp_ShaderColor,0.5f);

   glDepthMask(GL_FALSE);
	glDepthFunc(GL_GREATER);

	if(settings.pvr.Emulation.ModVolMode == 1)
	{
		//simply draw the volumes -- for debugging
		SetCull(0);
		glDrawArrays(GL_TRIANGLES,0,pvrrc.modtrig.used()*3);
		SetupMainVBO();
	}
	else
	{
		/*
		mode :
		normal trig : flip
		last *in*   : flip, merge*in* &clear from last merge
		last *out*  : flip, merge*out* &clear from last merge
		*/

		/*

			Do not write to color
			Do not write to depth

			read from stencil bits 1:0
			write to stencil bits 1:0
		*/

		glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE);

		if (settings.pvr.Emulation.ModVolMode == 2)
		{
			//simple single level stencil
			glEnable(GL_STENCIL_TEST);
         glStencilFunc(GL_ALWAYS, 0x1, 0x1);

         glStencilOp(GL_KEEP, GL_KEEP, GL_INVERT);
         glStencilMask(0x1);
			SetCull(0);
			glDrawArrays(GL_TRIANGLES,0,pvrrc.modtrig.used()*3);
		}
		else if (settings.pvr.Emulation.ModVolMode == 3)
		{
         glEnable(GL_STENCIL_TEST);
			//Full emulation
			//the *out* mode is buggy

			u32 mod_base=0; //cur start triangle
			u32 mod_last=0; //last merge

			u32 cmv_count= (pvrrc.global_param_mvo.used()-1);
			ISP_Modvol* params=pvrrc.global_param_mvo.head();

			//ISP_Modvol
			for (u32 cmv=0;cmv<cmv_count;cmv++)
			{

				ISP_Modvol ispc=params[cmv];
				mod_base=ispc.id;
				u32 sz=params[cmv+1].id-mod_base;

            if (sz == 0)
               continue;

				u32 mv_mode = ispc.DepthMode;

				if (mv_mode==0)	//normal trigs
				{
					SetMVS_Mode(0,ispc);
					//Render em (counts intersections)
					//verifyc(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST,sz,pvrrc.modtrig.data+mod_base,3*4));
					glDrawArrays(GL_TRIANGLES,mod_base*3,sz*3);
				}
				else if (mv_mode<3)
				{
					while(sz)
					{
						//merge and clear all the prev. stencil bits

						//Count Intersections (last poly)
						SetMVS_Mode(0,ispc);
						glDrawArrays(GL_TRIANGLES,mod_base*3,3);

						//Sum the area
						SetMVS_Mode(mv_mode,ispc);
						glDrawArrays(GL_TRIANGLES,mod_last*3,(mod_base-mod_last+1)*3);

						//update pointers
						mod_last=mod_base+1;
						sz--;
						mod_base++;
					}
				}
			}
		}
		//disable culling
		SetCull(0);
		//enable color writes
		glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);

		//black out any stencil with '1'
		glEnable(GL_BLEND);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		
		glEnable(GL_STENCIL_TEST);
      //only pixels that are Modvol enabled, and in area 1
      glStencilFunc(GL_EQUAL, 0x81, 0x81);
		
		//clear the stencil result bit
      glStencilMask(0x3); /* write to LSB */
      glStencilOp(GL_ZERO, GL_ZERO, GL_ZERO);

		//don't do depth testing
		glDisable(GL_DEPTH_TEST);

		SetupMainVBO();
		glDrawArrays(GL_TRIANGLE_STRIP,0,4);

		//Draw and blend
		//glDrawArrays(GL_TRIANGLES,pvrrc.modtrig.used(),2);
	}

	//restore states
   glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
}

/*
GL|ES 2
Slower, smaller subset of gl2

*Optimisation notes*
Keep stuff in packed ints
Keep data as small as possible
Keep vertex programs as small as possible
The drivers more or less suck. Don't depend on dynamic allocation, or any 'complex' feature
as it is likely to be problematic/slow
Do we really want to enable striping joins?

*Design notes*
Follow same architecture as the d3d renderer for now
Render to texture, keep track of textures in GL memory
Direct flip to screen (no vlbank/fb emulation)
Do we really need a combining shader? it is needlessly expensive for openGL | ES
Render contexts
Free over time? we actually care about ram usage here?
Limit max resource size? for psp 48k verts worked just fine

FB:
Pixel clip, mapping

SPG/VO:
mapping

TA:
Tile clip

*/

static bool gl_create_resources(void)
{
   u32 i;
   u32 cp_AlphaTest;
   u32 pp_ClipTestMode;
   u32 pp_UseAlpha;
   u32 pp_Texture;
   u32 pp_FogCtrl;
   u32 pp_IgnoreTexA;
   u32 pp_Offset;
   u32 pp_ShadInstr;
	PipelineShader* dshader  = 0;
   u32 compile              = 0;

	/* create VBOs */
	glGenBuffers(1, &vbo.geometry);
	glGenBuffers(1, &vbo.modvols);
	glGenBuffers(1, &vbo.idxs);
	glGenBuffers(1, &vbo.idxs2);

	memset(program_table,0,sizeof(program_table));

   for(cp_AlphaTest = 0; cp_AlphaTest <= 1; cp_AlphaTest++)
	{
      for (pp_ClipTestMode = 0; pp_ClipTestMode <= 2; pp_ClipTestMode++)
		{
			for (pp_UseAlpha = 0; pp_UseAlpha <= 1; pp_UseAlpha++)
			{
				for (pp_Texture = 0; pp_Texture <= 1; pp_Texture++)
				{
					for (pp_FogCtrl = 0; pp_FogCtrl <= 3; pp_FogCtrl++)
					{
						for (pp_IgnoreTexA = 0; pp_IgnoreTexA <= 1; pp_IgnoreTexA++)
						{
							for (pp_ShadInstr = 0; pp_ShadInstr <= 3; pp_ShadInstr++)
							{
								for (pp_Offset = 0; pp_Offset <= 1; pp_Offset++)
								{
                           int prog_id              = GetProgramID(
                                 cp_AlphaTest,
                                 pp_ClipTestMode,
                                 pp_Texture,
                                 pp_UseAlpha,
                                 pp_IgnoreTexA,
                                 pp_ShadInstr,
                                 pp_Offset,pp_FogCtrl);
									dshader                  = &program_table[prog_id];

									dshader->cp_AlphaTest    = cp_AlphaTest;
									dshader->pp_ClipTestMode = pp_ClipTestMode-1;
									dshader->pp_Texture      = pp_Texture;
									dshader->pp_UseAlpha     = pp_UseAlpha;
									dshader->pp_IgnoreTexA   = pp_IgnoreTexA;
									dshader->pp_ShadInstr    = pp_ShadInstr;
									dshader->pp_Offset       = pp_Offset;
									dshader->pp_FogCtrl      = pp_FogCtrl;
									dshader->program         = -1;
								}
							}
						}
					}
				}
			}
		}
	}

	modvol_shader.program        = gl_CompileAndLink(VertexShaderSource,ModifierVolumeShader);
	modvol_shader.scale          = glGetUniformLocation(modvol_shader.program, "scale");
	modvol_shader.sp_ShaderColor = glGetUniformLocation(modvol_shader.program, "sp_ShaderColor");
	modvol_shader.depth_scale    = glGetUniformLocation(modvol_shader.program, "depth_scale");

   if (settings.pvr.Emulation.precompile_shaders)
   {
      for (i=0;i<sizeof(program_table)/sizeof(program_table[0]);i++)
      {
         if (!CompilePipelineShader(	&program_table[i] ))
            return false;
      }
   }

	return true;
}

void tryfit(float* x,float* y)
{
	//y=B*ln(x)+A

	double sylnx=0,sy=0,slnx=0,slnx2=0;

	u32 cnt=0;

	for (int i=0;i<128;i++)
	{
		int rep=1;

		//discard values clipped to 0 or 1
		if (i<127 && y[i]==1 && y[i+1]==1)
			continue;

		if (i>0 && y[i]==0 && y[i-1]==0)
			continue;

		//Add many samples for first and last value (fog-in, fog-out -> important)
		if (i>0 && y[i]!=1 && y[i-1]==1)
			rep=10000;

		if (i<127 && y[i]!=0 && y[i+1]==0)
			rep=10000;

		for (int j=0;j<rep;j++)
		{
			cnt++;
			const double lnx = log((double)x[i]);
			sylnx += y[i] * lnx;
			sy += y[i];
			slnx += lnx;
			slnx2 += lnx * lnx;
		}
	}

	double a = 0, b = 0;
	if (slnx != 0)
	{
		b=(cnt*sylnx-sy*slnx)/(cnt*slnx2-slnx*slnx);
		a=(sy-b*slnx)/(cnt);


		//We use log2 and not ln on calculations	//B*log(x)+A
		//log2(x)=log(x)/log(2)
		//log(x)=log2(x)*log(2)
		//B*log(2)*log(x)+A
		b*=logf(2.0);
		/*
		float maxdev=0;
		for (int i=0;i<128;i++)
		{
			float diff=min(max(b*logf(x[i])/logf(2.0)+a,(double)0),(double)1)-y[i];
			maxdev=max((float)fabs((float)diff),(float)maxdev);
		}
		printf("FOG TABLE Curve match: maxdev: %.02f cents\n",maxdev*100);
		 */
	}
	ShaderUniforms.fog_coefs[0] = a;
	ShaderUniforms.fog_coefs[1] = b;
	//printf("%f\n",B*log(maxdev)/log(2.0)+A);
}

static void vertex_buffer_unmap(void)
{
   glBindBuffer(GL_ARRAY_BUFFER, 0);
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

#ifdef MSB_FIRST
#define INDEX_GET(a) (a^3)
#else
#define INDEX_GET(a) (a)
#endif

map<u64,TextureCacheData> TexCache;
typedef map<u64,TextureCacheData>::iterator TexCacheIter;

TextureCacheData *getTextureCacheData(TSP tsp, TCW tcw);

static void BindRTT(u32 addy, u32 fbw, u32 fbh, u32 channels, u32 fmt)
{
	FBT& rv=fb_rtt;

	if (rv.fbo)
      glDeleteFramebuffers(1,&rv.fbo);
	if (rv.tex)
      glDeleteTextures(1,&rv.tex);
	if (rv.depthb)
      glDeleteRenderbuffers(1,&rv.depthb);

	rv.TexAddr=addy>>3;

	/* Find the largest square POT texture that fits into the viewport */
   int fbh2 = 2;
   while (fbh2 < fbh)
      fbh2 *= 2;
   int fbw2 = 2;
   while (fbw2 < fbw)
      fbw2 *= 2;

	/* Get the currently bound frame buffer object. On most platforms this just gives 0. */
#if 0
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &m_i32OriginalFbo);
#endif

	/* Generate and bind a render buffer which will become a depth buffer shared between our two FBOs */
	glGenRenderbuffers(1, &rv.depthb);
	glBindRenderbuffer(RARCH_GL_RENDERBUFFER, rv.depthb);

	/*
		Currently it is unknown to GL that we want our new render buffer to be a depth buffer.
		glRenderbufferStorage will fix this and in this case will allocate a depth buffer
		m_i32TexSize by m_i32TexSize.
	*/

	glRenderbufferStorage(RARCH_GL_RENDERBUFFER, RARCH_GL_DEPTH24_STENCIL8, fbw2, fbh2);

	/* Create a texture for rendering to */
	glGenTextures(1, &rv.tex);
	glBindTexture(GL_TEXTURE_2D, rv.tex);

	glTexImage2D(GL_TEXTURE_2D, 0, channels, fbw2, fbh2, 0, channels, fmt, 0);

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	/* Create the object that will allow us to render to the aforementioned texture */
	glGenFramebuffers(1, &rv.fbo);
	glBindFramebuffer(RARCH_GL_FRAMEBUFFER, rv.fbo);

	/* Attach the texture to the FBO */
	glFramebufferTexture2D(RARCH_GL_FRAMEBUFFER, RARCH_GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rv.tex, 0);

	// Attach the depth buffer we created earlier to our FBO.
#if defined(HAVE_OPENGLES2) || defined(HAVE_OPENGLES1) || defined(OSX_PPC)
	glFramebufferRenderbuffer(RARCH_GL_FRAMEBUFFER, RARCH_GL_DEPTH_ATTACHMENT,
         RARCH_GL_RENDERBUFFER, rv.depthb);
   glFramebufferRenderbuffer(RARCH_GL_FRAMEBUFFER, RARCH_GL_STENCIL_ATTACHMENT,
         RARCH_GL_RENDERBUFFER, rv.depthb);
#else
   glFramebufferRenderbuffer(RARCH_GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
         RARCH_GL_RENDERBUFFER, rv.depthb);
#endif

	/* Check that our FBO creation was successful */
	GLuint uStatus = glCheckFramebufferStatus(RARCH_GL_FRAMEBUFFER);

	verify(uStatus == RARCH_GL_FRAMEBUFFER_COMPLETE);

   glViewport(0, 0, fbw, fbh);		// TODO CLIP_X/Y min?
}

static void DrawStrips(void)
{
	SetupMainVBO();
	//Draw the strips !

	//initial state
	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);

	//We use sampler 0
   glActiveTexture(GL_TEXTURE0);

	//Opaque
	//Nothing extra needs to be setup here
	/*if (!GetAsyncKeyState(VK_F1))*/
	DrawList<TA_LIST_OPAQUE, false>(pvrrc.global_param_op);

	//Alpha tested
	//setup alpha test state
	/*if (!GetAsyncKeyState(VK_F2))*/
	DrawList<TA_LIST_PUNCH_THROUGH, false>(pvrrc.global_param_pt);

	DrawModVols();

	//Alpha blended
	//Setup blending
	glEnable(GL_BLEND);

   glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

   if (settings.pvr.Emulation.AlphaSortMode == 0)
   {
      u32 count = pidx_sort.size();
      //if any drawing commands, draw them
      if (pvrrc.isAutoSort && count)
         DrawSorted(count);
      else
         DrawList<TA_LIST_TRANSLUCENT, false>(pvrrc.global_param_tr);
   }
   else if (settings.pvr.Emulation.AlphaSortMode == 1)
   {
      if (pvrrc.isAutoSort)
         SortPParams();
      DrawList<TA_LIST_TRANSLUCENT, true>(pvrrc.global_param_tr);
   }

   vertex_buffer_unmap();
}

static void ReadRTTBuffer()
{
	for (TexCacheIter i = TexCache.begin(); i != TexCache.end(); i++)
	{
		if (i->second.sa_tex == fb_rtt.TexAddr << 3)
			i->second.dirty = FrameCount;
	}

	u32 w = pvrrc.fb_X_CLIP.max - pvrrc.fb_X_CLIP.min + 1;
	u32 h = pvrrc.fb_Y_CLIP.max - pvrrc.fb_Y_CLIP.min + 1;

   u32 stride = FB_W_LINESTRIDE.stride * 8;
	if (stride == 0)
		stride = w * 2;
	else if (w * 2 > stride) {
    	// Happens for Virtua Tennis
    	w = stride / 2;
    }

   u32 size = w * h * 2;
   const u8 fb_packmode = FB_W_CTRL.fb_packmode;

   if (settings.pvr.RenderToTextureBuffer)
   {
      u32 tex_addr = fb_rtt.TexAddr << 3;

      // Manually mark textures as dirty and remove all vram locks before calling glReadPixels
      // (deadlock on rpi)
      for (TexCacheIter i = TexCache.begin(); i != TexCache.end(); i++)
      {
         if (i->second.sa_tex <= tex_addr + size - 1 && i->second.sa + i->second.size - 1 >= tex_addr) {
            i->second.dirty = FrameCount;
            if (i->second.lock_block != NULL) {
               libCore_vramlock_Unlock_block(i->second.lock_block);
               i->second.lock_block = NULL;
            }
         }
      }
      VArray2_UnLockRegion(&vram, 0, 2 * vram.size);


      glPixelStorei(GL_PACK_ALIGNMENT, 1);
      u16 *src = temp_tex_buffer;
      u16 *dst = (u16 *)&vram.data[fb_rtt.TexAddr << 3];

      GLint color_fmt, color_type;
      glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &color_fmt);
      glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &color_type);

      if (fb_packmode == 1 && stride == w * 2 && color_fmt == GL_RGB && color_type == GL_UNSIGNED_SHORT_5_6_5) {
         // Can be read directly into vram
         glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, dst);
      }
      else
      {
         const u16 kval_bit = (FB_W_CTRL.fb_kval & 0x80) << 8;
         const u8 fb_alpha_threshold = FB_W_CTRL.fb_alpha_threshold;

         u32 lines = h;
         while (lines > 0) {
            u8 *p = (u8 *)temp_tex_buffer;
            u32 chunk_lines = min((u32)sizeof(temp_tex_buffer), w * lines * 4) / w / 4;
            glReadPixels(0, h - lines, w, chunk_lines, GL_RGBA, GL_UNSIGNED_BYTE, p);

            for (u32 l = 0; l < chunk_lines; l++) {
               for (u32 c = 0; c < w; c++) {
                  switch(fb_packmode)
                  {
                     case 0: //0x0   0555 KRGB 16 bit  (default)	Bit 15 is the value of fb_kval[7].
                        *dst++ = (((p[0] >> 3) & 0x1F) << 10) | (((p[1] >> 3) & 0x1F) << 5) | ((p[2] >> 3) & 0x1F) | kval_bit;
                        break;
                     case 1: //0x1   565 RGB 16 bit
                        *dst++ = (((p[0] >> 3) & 0x1F) << 11) | (((p[1] >> 2) & 0x3F) << 5) | ((p[2] >> 3) & 0x1F);
                        break;
                     case 2: //0x2   4444 ARGB 16 bit
                        *dst++ = (((p[0] >> 4) & 0xF) << 8) | (((p[1] >> 4) & 0xF) << 4) | ((p[2] >> 4) & 0xF) | (((p[3] >> 4) & 0xF) << 12);
                        break;
                     case 3://0x3    1555 ARGB 16 bit    The alpha value is determined by comparison with the value of fb_alpha_threshold.
                        *dst++ = (((p[0] >> 3) & 0x1F) << 10) | (((p[1] >> 3) & 0x1F) << 5) | ((p[2] >> 3) & 0x1F) | (p[3] >= fb_alpha_threshold ? 0x8000 : 0);
                        break;
                  }
                  p += 4;
               }
               dst += (stride - w * 2) / 2;
            }
            lines -= chunk_lines;
         }
      }

      // Restore VRAM locks
      for (TexCacheIter i = TexCache.begin(); i != TexCache.end(); i++)
      {
         if (i->second.lock_block != NULL) {
            VArray2_LockRegion(&vram, i->second.sa_tex, i->second.sa + i->second.size - i->second.sa_tex);

            //TODO: Fix this for 32M wrap as well
            if (_nvmem_enabled() && VRAM_SIZE == 0x800000) {
               VArray2_LockRegion(&vram, i->second.sa_tex + VRAM_SIZE, i->second.sa + i->second.size - i->second.sa_tex);
            }
         }
      }
   }
   else
   {
      memset(&vram.data[fb_rtt.TexAddr << 3], '\0', size);
   }

   //dumpRtTexture(fb_rtt.TexAddr, w, h);

   if (w > 1024 || h > 1024) {
      glDeleteTextures(1, &fb_rtt.tex);
   }
   else
   {
      TCW tcw = { { TexAddr : fb_rtt.TexAddr, Reserved : 0, StrideSel : 0, ScanOrder : 1 } };
      switch (fb_packmode) {
         case 0:
         case 3:
            tcw.PixelFmt = 0;
            break;
         case 1:
            tcw.PixelFmt = 1;
            break;
         case 2:
            tcw.PixelFmt = 2;
            break;
      }
      TSP tsp = { 0 };
      for (tsp.TexU = 0; tsp.TexU <= 7 && (8 << tsp.TexU) < w; tsp.TexU++);
      for (tsp.TexV = 0; tsp.TexV <= 7 && (8 << tsp.TexV) < h; tsp.TexV++);

      TextureCacheData *texture_data = getTextureCacheData(tsp, tcw);
      if (texture_data->texID != 0)
         glDeleteTextures(1, &texture_data->texID);
      else {
         texture_data->Create(false);
         texture_data->lock_block = libCore_vramlock_Lock(texture_data->sa_tex, texture_data->sa + texture_data->size - 1, texture_data);
      }
      texture_data->texID = fb_rtt.tex;
      texture_data->dirty = 0;
   }
   fb_rtt.tex = 0;

	if (fb_rtt.fbo) { glDeleteFramebuffers(1,&fb_rtt.fbo); fb_rtt.fbo = 0; }
	if (fb_rtt.tex) { glDeleteTextures(1,&fb_rtt.tex); fb_rtt.tex = 0; }
	if (fb_rtt.depthb) { glDeleteRenderbuffers(1,&fb_rtt.depthb); fb_rtt.depthb = 0; }
	if (fb_rtt.stencilb) { glDeleteRenderbuffers(1,&fb_rtt.stencilb); fb_rtt.stencilb = 0; }
}

static bool RenderFrame(void)
{
	bool is_rtt=pvrrc.isRTT;

	//if (FrameCount&7) return;

	//Setup the matrix
   float vtx_min_fZ = 0.f;
	float vtx_max_fZ = pvrrc.fZ_max;

	//sanitise the values, now with NaN detection (for omap)
	//0x49800000 is 1024*1024. Using integer math to avoid issues w/ infs and nans
	if ((s32&)vtx_max_fZ<0 || (u32&)vtx_max_fZ>0x49800000)
		vtx_max_fZ=10*1024;


	//add some extra range to avoid clipping border cases
	vtx_min_fZ*=0.98f;
	vtx_max_fZ*=1.001f;

	//calculate a projection so that it matches the pvr x,y setup, and
	//a) Z is linearly scaled between 0 ... 1
	//b) W is passed though for proper perspective calculations

	/*
	PowerVR coords:
	fx, fy (pixel coordinates)
	fz=1/w

	(as a note, fx=x*fz;fy=y*fz)

	Clip space
	-Wc .. Wc, xyz
	x: left-right, y: bottom-top
	NDC space
	-1 .. 1, xyz
	Window space:
	translated NDC (viewport, glDepth)

	Attributes:
	//this needs to be cleared up, been some time since I wrote my rasteriser and i'm starting
	//to forget/mixup stuff
	vaX         -> VS output
	iaX=vaX*W   -> value to be interpolated
	iaX',W'     -> interpolated values
	paX=iaX'/W' -> Per pixel interpolated value for attribute


	Proper mappings:
	Output from shader:
	W=1/fz
	x=fx*W -> maps to fx after perspective divide
	y=fy*W ->         fy   -//-
	z=-W for min, W for max. Needs to be linear.



	umodified W, perfect mapping:
	Z mapping:
	pz=z/W
	pz=z/(1/fz)
	pz=z*fz
	z=zt_s+zt_o
	pz=(zt_s+zt_o)*fz
	pz=zt_s*fz+zt_o*fz
	zt_s=scale
	zt_s=2/(max_fz-min_fz)
	zt_o*fz=-min_fz-1
	zt_o=(-min_fz-1)/fz == (-min_fz-1)*W


	x=fx/(fx_range/2)-1		//0 to max -> -1 to 1
	y=fy/(-fy_range/2)+1	//0 to max -> 1 to -1
	z=-min_fz*W + (zt_s-1)  //0 to +inf -> -1 to 1

	o=a*z+c
	1=a*z_max+c
	-1=a*z_min+c

	c=-a*z_min-1
	1=a*z_max-a*z_min-1
	2=a*(z_max-z_min)
	a=2/(z_max-z_min)
	*/

	//float B=2/(min_invW-max_invW);
	//float A=-B*max_invW+vnear;

	//these should be adjusted based on the current PVR scaling etc params
	float dc_width=640;
	float dc_height=480;

	if (!is_rtt)
	{
		gcflip=0;
	}
	else
	{
		gcflip=1;

		//For some reason this produces wrong results
		//so for now its hacked based like on the d3d code
		/*
		u32 pvr_stride=(FB_W_LINESTRIDE.stride)*8;
		*/

		dc_width  = FB_X_CLIP.max-FB_X_CLIP.min+1;
		dc_height = FB_Y_CLIP.max-FB_Y_CLIP.min+1;
	}

	float scale_x=1, scale_y=1;

	float scissoring_scale_x = 1;

	if (!is_rtt)
	{
		scale_x=fb_scale_x;
		scale_y=fb_scale_y;

		//work out scaling parameters !
		//Pixel doubling is on VO, so it does not affect any pixel operations
		//A second scaling is used here for scissoring
		if (VO_CONTROL.pixel_double)
		{
			scissoring_scale_x  = 0.5f;
			scale_x            *= 0.5f;
		}
	}

	if (SCALER_CTL.hscale)
	{
      /* If the horizontal scaler is in use, we're (in principle) supposed to
    	 * divide everything by 2. However in the interests of display quality,
    	 * instead we want to render to the unscaled resolution and downsample
    	 * only if/when required.
    	 */
		scale_x*=2;
	}

	dc_width  *= scale_x;
	dc_height *= scale_y;

   glUseProgram(modvol_shader.program);

	/*

	float vnear=0;
	float vfar =1;

	float max_invW=1/vtx_min_fZ;
	float min_invW=1/vtx_max_fZ;

	float B=vfar/(min_invW-max_invW);
	float A=-B*max_invW+vnear;


	GLfloat dmatrix[16] =
	{
		(2.f/dc_width)  ,0                ,-(640/dc_width)              ,0  ,
		0               ,-(2.f/dc_height) ,(480/dc_height)              ,0  ,
		0               ,0                ,A                            ,B  ,
		0               ,0                ,1                            ,0
	};

	glUniformMatrix4fv(matrix, 1, GL_FALSE, dmatrix);

	*/

	/*
		Handle Dc to screen scaling
	*/
	float dc2s_scale_h = is_rtt ? (gles_screen_width / dc_width) : (gles_screen_height/480.0);
	float ds2s_offs_x  = is_rtt ? 0 : ((gles_screen_width-dc2s_scale_h*640)/2);

	//-1 -> too much to left
	ShaderUniforms.scale_coefs[0]=2.0f/(gles_screen_width/dc2s_scale_h*scale_x);
	ShaderUniforms.scale_coefs[1]= (is_rtt?2:-2) / dc_height;
   // FIXME CT2 needs 480 here instead of dc_height=512
	ShaderUniforms.scale_coefs[2]=1-2*ds2s_offs_x/(gles_screen_width);
	ShaderUniforms.scale_coefs[3]=(is_rtt?1:-1);


	ShaderUniforms.depth_coefs[0]=2/(vtx_max_fZ-vtx_min_fZ);
	ShaderUniforms.depth_coefs[1]=-vtx_min_fZ-1;
	ShaderUniforms.depth_coefs[2]=0;
	ShaderUniforms.depth_coefs[3]=0;

	//printf("scale: %f, %f, %f, %f\n", ShaderUniforms.scale_coefs[0],scale_coefs[1], ShaderUniforms.scale_coefs[2], ShaderUniforms.scale_coefs[3]);


	//VERT and RAM fog color constants
	u8* fog_colvert_bgra=(u8*)&FOG_COL_VERT;
	u8* fog_colram_bgra=(u8*)&FOG_COL_RAM;
	ShaderUniforms.ps_FOG_COL_VERT[0]=fog_colvert_bgra[INDEX_GET(2)]/255.0f;
	ShaderUniforms.ps_FOG_COL_VERT[1]=fog_colvert_bgra[INDEX_GET(1)]/255.0f;
	ShaderUniforms.ps_FOG_COL_VERT[2]=fog_colvert_bgra[INDEX_GET(0)]/255.0f;

	ShaderUniforms.ps_FOG_COL_RAM[0]=fog_colram_bgra [INDEX_GET(2)]/255.0f;
	ShaderUniforms.ps_FOG_COL_RAM[1]=fog_colram_bgra [INDEX_GET(1)]/255.0f;
	ShaderUniforms.ps_FOG_COL_RAM[2]=fog_colram_bgra [INDEX_GET(0)]/255.0f;


	//Fog density constant
	u8* fog_density=(u8*)&FOG_DENSITY;
	float fog_den_mant=fog_density[INDEX_GET(1)]/128.0f;  //bit 7 -> x. bit, so [6:0] -> fraction -> /128
	s32 fog_den_exp=(s8)fog_density[INDEX_GET(0)];
#ifndef MSB_FIRST
   float fog_den_float = fog_den_mant * powf(2.0f,fog_den_exp);
#endif
	ShaderUniforms.fog_den_float= fog_den_float;


	if (fog_needs_update)
	{
		fog_needs_update=false;
      //Get the coefs for the fog curve
		u8* fog_table=(u8*)FOG_TABLE;
		float xvals[128];
		float yvals[128];
		for (int i=0;i<128;i++)
		{
			xvals[i]=powf(2.0f,i>>4)*(1+(i&15)/16.f);
			yvals[i]=fog_table[i*4+1]/255.0f;
		}

		tryfit(xvals,yvals);
	}

	glUseProgram(modvol_shader.program);

	glUniform4fv(modvol_shader.scale, 1, ShaderUniforms.scale_coefs);
	glUniform4fv(modvol_shader.depth_scale, 1, ShaderUniforms.depth_coefs);


	GLfloat td[4]={0.5,0,0,0};

	ShaderUniforms.PT_ALPHA=(PT_ALPHA_REF&0xFF)/255.0f;

	for (u32 i=0;i<sizeof(program_table)/sizeof(program_table[0]);i++)
	{
		PipelineShader* s=&program_table[i];
		if (s->program == -1)
			continue;

		glUseProgram(s->program);

      set_shader_uniforms(&ShaderUniforms, s);
	}

	//setup render target first
	if (is_rtt)
	{
		GLuint channels,format;
		switch(FB_W_CTRL.fb_packmode)
		{
		case 0: //0x0   0555 KRGB 16 bit  (default)	Bit 15 is the value of fb_kval[7].
			channels=GL_RGBA;
			format=GL_UNSIGNED_SHORT_5_5_5_1;
			break;

		case 1: //0x1   565 RGB 16 bit
			channels=GL_RGB;
			format=GL_UNSIGNED_SHORT_5_6_5;
			break;

		case 2: //0x2   4444 ARGB 16 bit
			channels=GL_RGBA;
			format=GL_UNSIGNED_SHORT_4_4_4_4;
			break;

		case 3://0x3    1555 ARGB 16 bit    The alpha value is determined by comparison with the value of fb_alpha_threshold.
			channels=GL_RGBA;
			format=GL_UNSIGNED_SHORT_5_5_5_1;
			break;

		case 4: //0x4   888 RGB 24 bit packed
		case 5: //0x5   0888 KRGB 32 bit    K is the value of fk_kval.
		case 6: //0x6   8888 ARGB 32 bit
         fprintf(stderr, "Unsupported render to texture format: %d\n", FB_W_CTRL.fb_packmode);
         return false;
		case 7: //7     invalid
			die("7 is not valid");
			break;
		}
      //printf("RTT packmode=%d stride=%d - %d,%d -> %d,%d\n", FB_W_CTRL.fb_packmode, FB_W_LINESTRIDE.stride * 8,
 		//		FB_X_CLIP.min, FB_Y_CLIP.min, FB_X_CLIP.max, FB_Y_CLIP.max);	 		//		FB_X_CLIP.min, FB_Y_CLIP.min, FB_X_CLIP.max, FB_Y_CLIP.max);
		BindRTT(FB_W_SOF1 & VRAM_MASK, dc_width, dc_height, channels,format);
	}
   else
   {
      glViewport(0, 0, gles_screen_width, gles_screen_height);
   }

   if (!is_rtt && settings.rend.WideScreen)
      glClearColor(pvrrc.verts.head()->col[2]/255.0f,pvrrc.verts.head()->col[1]/255.0f,pvrrc.verts.head()->col[0]/255.0f,1.0f);
   else
      glClearColor(0,0,0,1.0f);

   glDepthMask(GL_TRUE);
   glStencilMask(0xFF);
   glClear(GL_COLOR_BUFFER_BIT|GL_STENCIL_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);

	if (UsingAutoSort())
		GenSorted();

	//move vertex to gpu

	//Main VBO
	glBindBuffer(GL_ARRAY_BUFFER, vbo.geometry);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vbo.idxs);

	glBufferData(GL_ARRAY_BUFFER,pvrrc.verts.bytes(),pvrrc.verts.head(),GL_STREAM_DRAW);

	glBufferData(GL_ELEMENT_ARRAY_BUFFER,pvrrc.idx.bytes(),pvrrc.idx.head(),GL_STREAM_DRAW);

	//Modvol VBO
	if (pvrrc.modtrig.used())
	{
		glBindBuffer(GL_ARRAY_BUFFER, vbo.modvols);
		glBufferData(GL_ARRAY_BUFFER,pvrrc.modtrig.bytes(),pvrrc.modtrig.head(),GL_STREAM_DRAW);
	}

	int offs_x=ds2s_offs_x+0.5f;
	//this needs to be scaled

	//not all scaling affects pixel operations, scale to adjust for that
	scale_x *= scissoring_scale_x;

#if 0
   //handy to debug really stupid render-not-working issues ...
   printf("SS: %dx%d\n", gles_screen_width, gles_screen_height);
   printf("SCI: %d, %f\n", pvrrc.fb_X_CLIP.max, dc2s_scale_h);
   printf("SCI: %f, %f, %f, %f\n", offs_x+pvrrc.fb_X_CLIP.min/scale_x,(pvrrc.fb_Y_CLIP.min/scale_y)*dc2s_scale_h,(pvrrc.fb_X_CLIP.max-pvrrc.fb_X_CLIP.min+1)/scale_x*dc2s_scale_h,(pvrrc.fb_Y_CLIP.max-pvrrc.fb_Y_CLIP.min+1)/scale_y*dc2s_scale_h);
#endif

   if (!is_rtt && settings.rend.WideScreen && pvrrc.fb_X_CLIP.min==0 && ((pvrrc.fb_X_CLIP.max+1)/scale_x==640) && (pvrrc.fb_Y_CLIP.min==0) && ((pvrrc.fb_Y_CLIP.max+1)/scale_y==480 ) )
   {
      glDisable(GL_SCISSOR_TEST);
   }
   else
   {
      glScissor(
            offs_x + pvrrc.fb_X_CLIP.min / scale_x,
            (pvrrc.fb_Y_CLIP.min / scale_y) * dc2s_scale_h,
            (pvrrc.fb_X_CLIP.max-pvrrc.fb_X_CLIP.min+1)/scale_x*dc2s_scale_h,
            (pvrrc.fb_Y_CLIP.max-pvrrc.fb_Y_CLIP.min+1)/scale_y*dc2s_scale_h
            );
      glEnable(GL_SCISSOR_TEST);
   }

	//restore scale_x
	scale_x /= scissoring_scale_x;

   DrawStrips();

	KillTex = false;
   
   if (is_rtt)
      ReadRTTBuffer();

	return !is_rtt;
}

void rend_set_fb_scale(float x,float y)
{
	fb_scale_x=x;
	fb_scale_y=y;
}

void co_dc_yield(void);

static int TexCacheLookups;
static int TexCacheHits;
static float LastTexCacheStats;

// Only use TexU and TexV from TSP in the cache key
const TSP TSPTextureCacheMask = { { TexV : 7, TexU : 7 } };
const TCW TCWTextureCacheMask = { { TexAddr : 0x1FFFFF, Reserved : 0, StrideSel : 0, ScanOrder : 0, PixelFmt : 7, VQ_Comp : 1, MipMapped : 1 } };

TextureCacheData *getTextureCacheData(TSP tsp, TCW tcw) {
	u64 key = ((u64)(tcw.full & TCWTextureCacheMask.full) << 32) | (tsp.full & TSPTextureCacheMask.full);

	TexCacheIter tx = TexCache.find(key);

	TextureCacheData* tf;
	if (tx != TexCache.end())
	{
		tf = &tx->second;
	}
	else //create if not existing
	{
		TextureCacheData tfc={0};
		TexCache[key] = tfc;

		tx=TexCache.find(key);
		tf=&tx->second;

		tf->tsp = tsp;
		tf->tcw = tcw;
	}

	return tf;
}

static GLuint gl_GetTexture(TSP tsp, TCW tcw)
{
   TexCacheLookups++;

	/* Lookup texture */
   TextureCacheData* tf = getTextureCacheData(tsp, tcw);

   if (tf->texID == 0)
		tf->Create(true);

	/* Update if needed */
	if (tf->NeedsUpdate())
		tf->Update();
   else
      TexCacheHits++;

	/* Update state for opts/stuff */
	tf->Lookups++;

	/* Return gl texture */
	return tf->texID;
}

text_info raw_GetTexture(TSP tsp, TCW tcw)
{
	text_info rv = { 0 };

	//lookup texture
	TextureCacheData* tf;
	u64 key = ((u64)(tcw.full & TCWTextureCacheMask.full) << 32) | (tsp.full & TSPTextureCacheMask.full);

	TexCacheIter tx = TexCache.find(key);

	if (tx != TexCache.end())
	{
		tf = &tx->second;
	}
	else //create if not existing
	{
		TextureCacheData tfc = { 0 };
		TexCache[key] = tfc;

		tx = TexCache.find(key);
		tf = &tx->second;

		tf->tsp = tsp;
		tf->tcw = tcw;
		tf->Create(false);
	}

	//update if needed
	if (tf->NeedsUpdate())
		tf->Update();

	//update state for opts/stuff
	tf->Lookups++;

	//return gl texture
	rv.height = tf->h;
	rv.width = tf->w;
	rv.pdata = tf->pData;
	rv.textype = tf->tex_type;
	
	
	return rv;
}

static void CollectCleanup(void)
{
   vector<u64> list;

   u32 TargetFrame = max((u32)120,FrameCount) - 120;

   for (TexCacheIter i=TexCache.begin();i!=TexCache.end();i++)
   {
      if ( i->second.dirty &&  i->second.dirty < TargetFrame)
         list.push_back(i->first);

      if (list.size() > 5)
         break;
   }

   for (size_t i=0; i<list.size(); i++)
   {
      TexCache[list[i]].Delete();
      TexCache.erase(list[i]);
   }
}

bool ProcessFrame(TA_context* ctx)
{
#ifndef TARGET_NO_THREADS
   slock_lock(ctx->rend_inuse);
#endif
   ctx->MarkRend();

   if (KillTex)
   {
      void killtex();
      killtex();
      printf("Texture cache cleared\n");
   }

   if (!ta_parse_vdrc(ctx))
      return false;

   CollectCleanup();

   return true;
}

struct glesrend : Renderer
{
	bool Init()
   {
      libCore_vramlock_Init();

      glsm_ctl(GLSM_CTL_STATE_SETUP, NULL);

      if (!gl_create_resources())
         return false;

      return true;
   }
	void Resize(int w, int h) { gles_screen_width=w; gles_screen_height=h; }
	void Term() { libCore_vramlock_Free(); }

	bool Process(TA_context* ctx)
   {
      return ProcessFrame(ctx);
   }
	bool Render()
   {
      glsm_ctl(GLSM_CTL_STATE_BIND, NULL);
      return RenderFrame();
   }

	void Present()
   {
      glsm_ctl(GLSM_CTL_STATE_UNBIND, NULL);
      co_dc_yield();
   }

	virtual u32 GetTexture(TSP tsp, TCW tcw) {
		return gl_GetTexture(tsp, tcw);
	}
};

Renderer* rend_GLES2() { return new glesrend(); }


/*
Textures

Textures are converted to native OpenGL textures
The mapping is done with tcw:tsp -> GL texture. That includes stuff like
filtering/ texture repeat

To save space native formats are used for 1555/565/4444 (only bit shuffling is done)
YUV is converted to 565 (some loss of quality on that)
PALs are decoded to their unpaletted format, 8888 is downcasted to 4444

Mipmaps
	not supported for now

Compression
	look into it, but afaik PVRC is not realtime doable
*/

void killtex(void)
{
	for (TexCacheIter i=TexCache.begin();i!=TexCache.end();i++)
		i->second.Delete();

	TexCache.clear();
}

void rend_text_invl(vram_block* bl)
{
	TextureCacheData* tcd = (TextureCacheData*)bl->userdata;
	tcd->dirty=FrameCount;
	tcd->lock_block=0;

	libCore_vramlock_Unlock_block_wb(bl);
}