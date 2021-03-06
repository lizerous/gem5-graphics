/*
 * Copyright (c) 2011 Mark D. Hill and David A. Wood
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __CUDA_CORE_HH__
#define __CUDA_CORE_HH__

#include <map>
#include <queue>
#include <set>

#include "cpu/translation.hh"
#include "cuda-sim/ptx.tab.h"
#include "cuda-sim/ptx_ir.h"
#include "cuda-sim/ptx_sim.h"
#include "gpgpu-sim/mem_fetch.h"
#include "gpgpu-sim/shader.h"
#include "gpu/atomic_operations.hh"
#include "gpu/shader_tlb.hh"
#include "mem/mem_object.hh"
#include "params/CudaCore.hh"

class CudaGPU;

/**
 *  Wrapper class for the shader cores in GPGPU-Sim. Shader memory references
 *  to GPU const, global and instruction memory go through this wrapper to the
 *  gem5-side memory hierarchy.
 *
 *  NOTE: A CUDA core is equivalent to an NVIDIA streaming multiprocessor (SM)
 */
class CudaCore : public MemObject
{
  protected:
    typedef CudaCoreParams Params;

    // Functions to translate GPGPU-Sim values to gem5-gpu values
    static AtomicOpRequest::Operation
    getAtomOpType(unsigned gpgpu_sim_value) {
        switch (gpgpu_sim_value) {
          case ATOMIC_CAS:
            return AtomicOpRequest::ATOMIC_CAS_OP;
          case ATOMIC_ADD:
            return AtomicOpRequest::ATOMIC_ADD_OP;
          case ATOMIC_INC:
            return AtomicOpRequest::ATOMIC_INC_OP;
          case ATOMIC_MIN:
            return AtomicOpRequest::ATOMIC_MIN_OP;
          case ATOMIC_MAX:
            return AtomicOpRequest::ATOMIC_MAX_OP;
          default:
            panic("Unknown atomic type: %llu\n", gpgpu_sim_value);
            break;
        }
        return AtomicOpRequest::ATOMIC_INVALID_OP;
    }

    static AtomicOpRequest::DataType
    getDataType(unsigned gpgpu_sim_value) {
        switch (gpgpu_sim_value) {
          case S32_TYPE:
            return AtomicOpRequest::S32_TYPE;
          case U32_TYPE:
            return AtomicOpRequest::U32_TYPE;
          case F32_TYPE:
            return AtomicOpRequest::F32_TYPE;
          case B32_TYPE:
            return AtomicOpRequest::B32_TYPE;
          default:
            panic("Unknown atomic data type: %llu\n", gpgpu_sim_value);
            break;
        }
        return AtomicOpRequest::INVALID_TYPE;
    }

    /**
     * Port for sending a receiving memory accesses
     * Required for implementing MemObject
     */
    enum class CorePortType {Inst, Tex};
    class CoreCachePort : public MasterPort
    {
      friend class CudaCore;

      private:
        // Pointer back to shader core for callbacks
        CudaCore *core;
        CorePortType type;

      public:
        CoreCachePort(const std::string &_name, CudaCore *_core, CorePortType _type)
        : MasterPort(_name, _core), core(_core), type(_type) {}

      protected:
        virtual bool recvTimingResp(PacketPtr pkt);
        virtual void recvReqRetry();
        virtual Tick recvAtomic(PacketPtr pkt);
        virtual void recvFunctional(PacketPtr pkt);
    };

    // Instantiations of above port
    CoreCachePort instPort;
    CoreCachePort texPort;

    class CachePortRetryEvent : public Event
    {
    private:
        CudaCore * core;
        CorePortType type;
    public:
        CachePortRetryEvent(CudaCore * _core, CorePortType _type):
           core(_core), type(_type){
            setFlags(Event::AutoDelete);
        }
        void process() {
           if(type == CorePortType::Inst) {
              core->handleInstRetry();
           } else if (type == CorePortType::Tex){
              core->handleTexRetry();
           } else {
              fatal("Unexpected port type");
           }
        }
    };
    

    /**
     * Port to send packets to the load/store queue and coalescer
     */
    class LSQPort : public MasterPort
    {
       friend class CudaCore;

      private:
        CudaCore *core;
        int idx;

      public:
        LSQPort(const std::string &_name, CudaCore *_core, int _idx)
        : MasterPort(_name, _core), core(_core), idx(_idx) {}

      protected:
        virtual bool recvTimingResp(PacketPtr pkt);
        virtual void recvReqRetry();
    };
    // Ports for each of the GPU lanes
    std::vector<LSQPort*> lsqPorts;
    std::vector<LSQPort*> texPorts;
    std::vector<LSQPort*> constPorts;
    std::vector<LSQPort*> zPorts;

    /**
     * A port to send control commands to the LSQ. Currently, this is used
     * to send the flush command on kernel boundaries. Functions more like a
     * register for transmitting control logic than a true port, so receive
     * functions do not block.
     */
    class LSQControlPort : public MasterPort
    {
      friend class CudaCore;

      private:
        CudaCore *core;

      public:
        LSQControlPort(const std::string &_name, CudaCore *_core)
        : MasterPort(_name, _core), core(_core) {}

      protected:
        virtual bool recvTimingResp(PacketPtr pkt);
        virtual void recvReqRetry();
    };
    LSQControlPort lsqControlPort;
    LSQControlPort texControlPort;
    LSQControlPort constControlPort;
    LSQControlPort zControlPort;

    // Port that is blocked. If -1 then no port is blocked.
    struct LSQCntrlPortType_enum {
       enum LSQCntrlPortType {LSQ, TEX, CONST, Z, SIZE };
    };
    typedef LSQCntrlPortType_enum::LSQCntrlPortType LSQCntrlPortType;
    int writebackBlocked[LSQCntrlPortType::SIZE];

    class SenderState : public Packet::SenderState {
    public:
        SenderState(warp_inst_t _inst) : inst(_inst) {}
        warp_inst_t inst;
    };

      class VpoMasterPort : public MasterPort
    {
      public:
      enum class Type {DataPort, DistributionPort};
      VpoMasterPort(const std::string &_name, CudaCore * _core)
         : MasterPort(_name, _core), core(_core) {}


      protected:
      virtual bool recvTimingResp(PacketPtr pkt);
      virtual void recvReqRetry();
      virtual Tick recvAtomic(PacketPtr pkt);
      virtual void recvFunctional(PacketPtr pkt);

      private:
      CudaCore* core;
    };

    VpoMasterPort vpoWritePort;

    std::queue<PacketPtr> vpoWritePkts;

    //used to signal when an attrib has been committed to the cache
    //upon receiving a response 
    struct LastAttribPkt{
       unsigned warpId;
       unsigned activeCount;
       LastAttribPkt(){
          warpId = -1;
          activeCount = -1;
       }
       LastAttribPkt(unsigned wid, unsigned at){
          warpId = wid;
          activeCount = at;
       }
    };
    std::map<PacketPtr, LastAttribPkt> lastAttribPkts;

    bool recvVpoTimingResp(PacketPtr pkt);
    void recvVpoReqRetry();

    const Params * params() const {
        return dynamic_cast<const Params *>(_params);
    }
    const CudaCoreParams *_params;

    MasterID dataMasterId;
    MasterID instMasterId;
    MasterID texMasterId;
    MasterID constMasterId;
    MasterID zMasterId;
    MasterID vpoVertWriteMasterId;

  private:

    // ID for this CUDA core, should match the id in GPGPU-Sim
    int id;

    // Number of threads in the warp, also the number of lanes per SM
    int warpSize;

    // Stalled because a memory request called recvReqRetry, usually because
    // a queue filled up
    bool stallOnICacheRetry;
    bool stallOnTexCacheRetry;

    // Holds all outstanding addresses, maps from address to mf object used
    // mostly for acking GPGPU-Sim
    std::map<Addr,mem_fetch *> instBusyCacheLineAddrs;
    std::map<Addr,mem_fetch *> texBusyCacheLineAddrs;
    std::map<Addr, mem_fetch*> zBusyCacheLineAddrs;

    // Holds instruction packets that need to be retried
    std::list<PacketPtr> retryInstPkts;
    
    // Holds texture packets that need to be retried
    std::list<PacketPtr> retryTexPkts;

    // The TLB to translate instruction addresses
    ShaderTLB *itb;
    
    // The TLB to translate texture addresses
    ShaderTLB *ttb;

    // Point to GPU this CUDA core is part of
    CudaGPU *cudaGPU;

    // Pointer to GPGPU-Sim shader this CUDA core is a proxy for
    shader_core_ctx *shaderImpl;

    // Indicate whether an outstanding memory fence must be signaled back to
    // the shader (e.g. to unblock warp issue)
    std::vector<bool> needsFenceUnblock;
    unsigned maxNumWarpsPerCore;

    // if true then need to signal GPGPU-Sim once cleanup is done
    bool signalKernelFinish;

    // Can we issue an inst or tex cache request this cycle?
    int isCacheResourceAvailable(Addr addr, std::map<Addr,mem_fetch *>* cacheMap);

    Cycles lastActiveCycle;

    std::map<unsigned, bool> coreCTAActive;
    std::map<unsigned, std::vector<Tick> > coreCTAActiveStats;
    Cycles beginActiveCycle;
    int activeCTAs;
    int sentFlushReqs;
    int receivedFlushResp;

  public:
    // Constructor and deconstructor
    CudaCore(const Params *p);
    ~CudaCore();

    // Required for implementing MemObject
    virtual BaseMasterPort& getMasterPort(const std::string &if_name,
          PortID idx = InvalidPortID);
    virtual BaseSlavePort& getSlavePort(const std::string& if_name,
          PortID idx = InvalidPortID);


    // For checkpoint restore (empty unserialize)
    virtual void unserialize(CheckpointIn &cp);

    // Perform initialization. Called from SPA
    void initialize();

    /** This function is used by the page table walker to determine if it could
     * translate the a pending request or if the underlying request has been
     * squashed. This always returns false for the GPU as it never
     * executes any instructions speculatively.
     * @ return Is the current instruction squashed?
     */
    bool isSquashed() const { return false; }

    // Instruction and texture memory access functions:
    void xCacheFetch(Addr addr, mem_fetch *mf, const char * type,
        MasterID masterId, std::map<Addr,mem_fetch *> * busyCacheLineAddrs,
        ShaderTLB * tlb, Request::FlagsType flagType,
        Request::FlagsType gpuFlagType);
    
    // Initializes an instruction fetch from gem5-side memory
    void icacheFetch(Addr a, mem_fetch *mf);
    
    // Initializes a texture fetch from gem5-side memory
    void texCacheFetch(Addr a, mem_fetch *mf);
    
    // Initializes a z cache fetch from gem5-side memory
    void zCacheFetch(Addr a, mem_fetch *mf);

    bool texCacheResAvailabe(Addr a);

    // Required for instruction/texture cache translation.
    void finishTranslation(WholeTranslationState *state);
    
  private:
    // Sends an instruction memory access to the cache
    void sendInstAccess(PacketPtr pkt);
    
    // Sends a texture memory access to the cache
    void sendTexAccess(PacketPtr pkt);

    // Handle an instruction port retry request
    void handleInstRetry();
    
    // Handle a texture port retry request
    void handleTexRetry();

  public:
    // Receive and complete an instruction fetch
    void recvInstResp(PacketPtr pkt);
    
    // Receive and complete a texture fetch
    void recvTexResp(PacketPtr pkt);
     
    /**
     * This function is the main entrypoint from GPGPU-Sim
     * This function parses the instruction from GPGPU-Sim and issues the
     * memory request to the LSQ on a per-lane basis.
     * @return true if stall
     */
    bool executeMemOp(const warp_inst_t &inst);

    /**
     * The specified lane is returning a packet from the ShaderLSQ to be
     * handled as appropriate (e.g. LD instructions return data, fences may
     * need to be signaled to the appropriate thread)
     */
    bool recvLSQDataResp(PacketPtr pkt, int lane_id);

    /**
     * The ShaderLSQ is returning a control signal. This currently handles
     * flushes, but may handle other situations that need to block/unblock
     * warp issue.
     */
    void recvLSQControlResp(PacketPtr pkt);

    /**
     * GPGPU-Sim calls this function when the writeback register in its ld/st
     * unit is cleared. If there is a lsqPort blocked, it may now try again
     */
    void writebackClear();

    /**
     * Flush the core of all pending instructions,
     * This is currently used to force the LSQ to flush on kernel end
     */
    void flush();

    /**
     * Called from GPGPU-Sim when a kernel completes on this shader
     * Must signal back to GPGPU-Sim after all cleanup is done
     */
    void finishKernel();

    // For counting statistics
    Stats::Scalar numLocalLoads;
    Stats::Scalar numLocalStores;
    Stats::Scalar numSharedLoads;
    Stats::Scalar numSharedStores;
    Stats::Scalar numParamKernelLoads;
    Stats::Scalar numParamLocalLoads;
    Stats::Scalar numParamLocalStores;
    Stats::Scalar numConstLoads;
    Stats::Scalar numTexLoads;
    Stats::Scalar numZLoads;
    Stats::Scalar numZStores;
    Stats::Scalar numGlobalLoads;
    Stats::Scalar numGlobalStores;
    Stats::Scalar numSurfLoads;
    Stats::Scalar numGenericLoads;
    Stats::Scalar numGenericStores;
    Stats::Scalar numDataCacheRequests;
    Stats::Scalar numDataCacheRetry;
    Stats::Scalar numInstCacheRequests;
    Stats::Scalar numInstCacheRetry;
    Stats::Vector instCounts;
    Stats::Scalar numTexCacheRequests;
    Stats::Scalar numTexCacheRetry;
    Stats::Scalar activeCycles;
    Stats::Scalar notStalledCycles;
    Stats::Scalar instInstances;
    Stats::Formula instPerCycle;
    Stats::Scalar numKernelsCompleted;
    void regStats();

    void record_ld(memory_space_t space);
    void record_st(memory_space_t space);
    void record_inst(int inst_type);
    void record_block_issue(unsigned hw_cta_id);
    void record_block_commit(unsigned hw_cta_id);
    void printCTAStats(std::ostream& out);
};


#endif

