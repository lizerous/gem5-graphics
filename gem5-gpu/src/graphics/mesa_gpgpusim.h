#ifndef __MESA_GPGPUSIM_H_
#define __MESA_GPGPUSIM_H_



#undef NDEBUG
#include <cassert> 
#include <cstdio>
#include <cstdlib>
#include <string>
#include <iostream>
#include <vector>
#include <mutex>

extern "C" {
#include "math/m_xform.h"
#include "main/mtypes.h"
#include "s_span.h"
#include "tnl/t_context.h"
#include "program/prog_statevars.h"
}

#define SKIP_API_GEM5
#include "api/cuda_syscalls.hh"
#include "graphics/gpgpusim_calls.h"


typedef unsigned char byte;
const int VECTOR_SIZE = 4;
const int CUDA_FLOAT_SIZE = 4;
class CudaGPU;

enum shaderType_t{
    NO_SHADER,
    VERTEX_PROGRAM,
    FRAGMENT_PROGRAM
};

//gpgpusim calls
extern "C" void gpgpusimLoadShader(int shaderType, std::string arbFile, std::string ptxFile);

//mesa calls we use
extern "C" GLboolean GLAPIENTRY _mesa_IsEnabled(GLenum cap);
extern "C" void get_uniform_rows_cols(const struct gl_program_parameter *p, GLint *rows, GLint *cols);
extern "C" tnl_render_func* init_run_render(struct gl_context *ctx);
extern "C" void finalize_run_render(struct gl_context *ctx);
extern "C" void run_render_prim(struct gl_context *ctx, tnl_render_func *tab, struct vertex_buffer *VB, GLuint primId);
extern "C" void _mesa_fetch_state(struct gl_context *ctx, const gl_state_index[], GLfloat *value);
extern "C" void copy_vp_results(struct gl_context *ctx, struct vertex_buffer *VB, struct vp_stage_data *store, struct gl_vertex_program *program);
extern "C" GLboolean do_ndc_cliptest(struct gl_context *ctx, struct vp_stage_data *store, GLuint primId);
/////////

typedef void (*PutRowFunc)(struct gl_context *ctx, struct gl_renderbuffer *rb, GLuint count, GLint x, GLint y, const void *values, const GLubyte *mask);
typedef void (*GetRowFunc)(struct gl_context *ctx, struct gl_renderbuffer *rb, GLuint count, GLint x, GLint y, void *values);

struct fragmentData_t {
   fragmentData_t(): passedDepth(false) {}
   GLfloat attribs[FRAG_ATTRIB_MAX][4];
   unsigned intPos[3];
   bool passedDepth;
};


class RasterTile {
   public:
      void push_back(fragmentData_t frag){ m_fragments.push_back(frag); }

      unsigned size() const { return m_fragments.size();} 
      
      fragmentData_t& operator[] (const int index)
      {
         return m_fragments[index];
      }

      unsigned getActiveFragmentsCount() {
         unsigned activeCount = 0;
         for(int i=0; i<m_fragments.size(); i++){
            if(m_fragments[i].passedDepth){
               activeCount++;
            }
         }
         return activeCount;
      }

   private:
      std::vector<fragmentData_t> m_fragments;
};


typedef std::vector<RasterTile* > RasterTiles;

enum RasterDirection {
    VerticalRaster,
    HorizontalRaster,
    BlockedHorizontal,
    HilbertOrder
};

enum class DepthSize : uint32_t { Z16 = 2, Z32 = 4 };

struct stage_shading_info_t {
    enum class GraphicsPass { NONE , Vertex, Fragment};
    GraphicsPass currentPass;
    unsigned launched_threads;
    unsigned completed_threads;
    bool doneEarlyZ;
    RasterTiles * earlyZTiles;
    bool render_init;
    tnl_render_func* renderFunc;
    bool initStageKernelPtr;
    unsigned * primMap;
    unsigned * primCountMap;
    bool finishStageShaders;
    float * deviceVertsAttribs;
    cudaStream_t cudaStream;
    void* allocAddr;
    void* vertCodeAddr;
    void* fragCodeAddr;

    stage_shading_info_t() {
        primMap = NULL;
        primCountMap = NULL;
        earlyZTiles = NULL;
        clear();
    }

    void clear() {
        currentPass = GraphicsPass::NONE;
        launched_threads = 0;
        completed_threads = 0;
        doneEarlyZ = true;
        render_init = true;
        renderFunc = NULL;
        allocAddr = NULL;
        vertCodeAddr = NULL;
        fragCodeAddr = NULL;
        deviceVertsAttribs = NULL;
        if(primMap!=NULL){ delete [] primMap;  primMap=NULL;}
        if(primCountMap!=NULL) { delete [] primCountMap; primCountMap= NULL;}
        if(earlyZTiles!=NULL) { assert(0); } //should be cleared when earlyZ is done
    }
};


class primitiveFragmentsData_t {
public:
    bool hasStream(void* stream);
    float getFragmentData(unsigned threadID, unsigned attribID, unsigned attribIndex, void * stream);
    int getFragmentDataInt(unsigned threadID, unsigned attribID, unsigned attribIndex, void * stream);
    void addFragment(fragmentData_t fd);
    void addStreamCall(void* stream, unsigned fragmentsCount);
    unsigned getFragmentsCount();
    void sortFragmentsInRasterOrder (unsigned frameHeight, unsigned frameWidth,
        const unsigned tileH, const unsigned tileW,
        const unsigned blockH, const unsigned blockW, const RasterDirection rasterDir);
    RasterTiles* sortFragmentsInTiles(unsigned frameHeight, unsigned frameWidth,
        const unsigned tileH, const unsigned tileW,
        const unsigned blockH, const unsigned blockW, const RasterDirection rasterDir);
    void clear();
private:
    std::vector<void*> associatedStreams; //which streams are belonging to this primitive
    std::vector<unsigned> streamsStartIndex; //where every stream data of this primitive starts
    std::vector<fragmentData_t> fragmentsData; //the fragment shading data of this primitive
};


class renderData_t {
public:
    renderData_t();
    ~renderData_t();
    
    //mesa calls
    bool GPGPUSimSimulationActive();
    bool GPGPUSimSkipCpFrames();
    void endOfFrame();
    void initializeCurrentDraw (gl_context * ctx);
    void finilizeCurrentDraw();
    void startPrimitive();
    GLboolean doVertexShading(GLvector4f ** inputParams, vp_stage_data * stage);
    bool m_flagEndVertexShader;
    void endVertexShading(CudaGPU * cudaGPU);
    unsigned int doFragmentShading();
    bool m_flagEndFragmentShader;
    void endFragmentShading();
    void addFragmentsSpan(SWspan* span);
    
    //gpgpusim calls
    bool isDepthTestEnabled();
    bool isBlendingEnabled();
    void getBlendingMode(GLenum * src, GLenum * dst, GLenum* srcAlpha, GLenum * dstAlpha, GLenum* eqnRGB, GLenum* eqnAlpha, GLfloat * blendColor);
    void initParams(unsigned int startFrame, unsigned int endFrame, int startDrawcall, unsigned int endDrawcall, unsigned int tile_H, unsigned int tile_W,
          unsigned int block_H, unsigned int block_W, unsigned blendingMode, unsigned depthMode, unsigned cptStartFrame, unsigned cptEndFrame, unsigned cptPeroid, bool skipCpFrames, char* outdir);
    GLuint getScreenWidth(){return m_bufferWidth;}
    GLuint getRBSize(){return m_bufferWidth*m_bufferHeight;}
    float getFragmentData(unsigned threadID, unsigned attribID, unsigned attribIndex, void * stream);
    int getFragmentDataInt(unsigned threadID, unsigned attribID, unsigned attribIndex, void * stream);
    int getVertexDataInt(unsigned threadID, unsigned attribID, unsigned attribIndex, void * stream);
    void writeVertexResult(unsigned threadID, unsigned resAttribID, unsigned attribIndex, float data);
    void checkGraphicsThreadExit(void * kernelPtr, unsigned tid);
    void setTcInfo(int pid, int tid){m_tcPid = pid; m_tcTid=tid;}

    //gem5 calls
    void checkEndOfShader(CudaGPU * cudaGPU);
    void doneEarlyZ(); 
    void launchFragmentTile(RasterTile * rasterTile);
private:
    //private funcs
    bool useInShaderBlending() const;
    void addPrimitive();
    void addStreamCall(void * stream, unsigned fragmentsCount);
    unsigned getLastPrimitiveFragmentsCount();
    void sortFragmentsInRasterOrder(unsigned tileH, unsigned tileW, unsigned blockH, unsigned blockW, RasterDirection dir);
    void runEarlyZ(CudaGPU * cudaGPU, unsigned tileH, unsigned tileW, unsigned blockH, unsigned blockW, RasterDirection dir);
    void generateVertexCode();
    void generateFragmentCode(DepthSize);
    void addFragment(fragmentData_t fragmentData);
    void endDrawCall();
    std::string getDefaultVertexShader();
    std::string getDefaultFragmentShader();
    void setAllTextures(void** fatCubinHandle);
    void setTextureUnit(void** fatCubinHandle, int textureNumber, int textureType, int h, int w, byte * inData,
        int wrapS, int wrapT, int minFilter, int magFilter, GLenum target);
    void putDataOnImageBuffer();
    void writeDrawBuffer(std::string time, byte * frame, int size, unsigned w, unsigned h, std::string extOrder);
    byte* setRenderBuffer();
    byte* setDepthBuffer(DepthSize activeDbSize, DepthSize actualDbSize);
    void writeTexture(byte* data, unsigned size, unsigned texNum, unsigned h, unsigned w, std::string typeEx);
    void copyStateData(void** fatCubinHandle);
    
    void setMesaCtx(struct gl_context * ctx){m_mesaCtx=ctx;}
    void incCurrentFrame(){
       m_currentFrame++;
       m_drawcall_num = 0;
       checkpoint();
       checkExitCond();
    }
    void incDrawcallNum(){
       m_drawcall_num++;
       checkExitCond();
    }

    void checkExitCond();
    void checkpoint();
    unsigned int getCurrentFrame(){return m_currentFrame;}
    long long unsigned getDrawcallNum(){return m_drawcall_num;}
    const char* getCurrentShaderId(int shaderType);
    std::string getCurrentShaderName(int shaderType){
        if(shaderType==VERTEX_PROGRAM)
            return ("vp" + std::to_string(m_drawcall_num));
        if(shaderType==FRAGMENT_PROGRAM)
            return ("fp" + std::to_string(m_drawcall_num));
        //only two types
        assert(0);
    }
    unsigned getTileH(){return m_tile_H;}
    unsigned getTileW(){return m_tile_W;}
    unsigned getBlockH(){return m_block_H;}
    unsigned getBlockW(){return m_block_W;}
    
    unsigned getBufferSize(){return m_colorBufferSize;}
    GLuint getBufferWidth(){return m_bufferWidth;}
    GLuint getBufferHeight(){return m_bufferHeight;}
    struct gl_context * getMesaCtx(){return m_mesaCtx;}
    struct gl_renderbuffer * getMesaBuffer(){return m_mesaColorBuffer;}
    std::string getIntFolder(){return m_intFolder;}
    std::string getFbFolder(){return m_fbFolder;}
    bool useDefaultShaders(){return m_useDefaultShaders;}
    byte* getDeviceData(){return m_deviceData;}
    byte** getpDeviceData(){return &m_deviceData;}
    void putRow(struct gl_context *ctx, struct gl_renderbuffer *rb, GLuint count, GLint x, GLint y, const void *values, const GLubyte *mask){
        m_colorBufferPutRow(ctx, rb, count, x, y, values, mask);
    }
    std::string getShaderPTXInfo(std::string arbFileName, std::string functionName);
    void* getShaderFatBin(std::string vertexShader, std::string fragmentShader);
    gl_state_index getParamStateIndexes(gl_state_index index);
    
    //private data
private:
    std::string vGlslPrfx;
    std::string vARBPrfx;
    std::string vPTXPrfx;
    std::string fGlslPrfx;
    std::string fARBPrfx;
    std::string fPTXPrfx;
    std::string fPtxInfoPrfx;
    vp_stage_data * vertexStageData;
    byte* m_deviceData;
    std::string m_intFolder;
    std::string m_fbFolder;
    unsigned int m_startFrame;
    unsigned int m_endFrame;
    unsigned int m_cptStartFrame;
    unsigned int m_cptEndFrame;
    unsigned int m_cptPeroid;
    bool m_skipCpFrames;
    unsigned int m_cptNextFrame;
    unsigned int m_currentFrame;
    int m_startDrawcall; 
    unsigned int m_endDrawcall;
    bool m_useDefaultShaders;
    uint64_t m_colorBufferSize;
    uint64_t m_depthBufferSize;
    DepthSize m_depthSize;
    GLuint m_bufferWidth;
    GLuint m_bufferHeight;
    GLuint m_depthBufferWidth;
    GLuint m_depthBufferHeight;
    struct gl_context * m_mesaCtx;
    struct gl_renderbuffer * m_mesaColorBuffer;
    struct gl_renderbuffer * m_mesaDepthBuffer;
    PutRowFunc m_colorBufferPutRow;
    PutRowFunc m_depthBufferPutRow;
    unsigned int m_tile_H;
    unsigned int m_tile_W;
    unsigned int m_block_H;
    unsigned int m_block_W;
    GLuint m_bufferWidthx4;
    long long unsigned m_drawcall_num;
    bool currentFrameHasShader;
    bool m_inShaderBlending; //1 in shader, 0 in z-unit
    bool m_inShaderDepth; //1 in shader, 0 in z-unit
    int callsCount;
    std::vector<primitiveFragmentsData_t> primitivesData;
    stage_shading_info_t m_sShading_info;
    std::vector<textureReference*> textureRefs;
    cudaArray* textureArray[MAX_COMBINED_TEXTURE_IMAGE_UNITS];
    void** lastFatCubinHandle;
    __cudaFatCudaBinary* lastFatCubin;
    int m_tcPid;
    int m_tcTid;
    std::string m_outdir;
};

extern renderData_t g_renderData;

class Utils {
   public:
      static byte* RGB888_to_RGBA888(byte* rgb, int size, byte alpha=255 /*fully opaque*/);
};

#endif //MESA_GPGPUSIM_H
