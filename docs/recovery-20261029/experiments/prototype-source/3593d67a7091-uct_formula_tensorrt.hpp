// Included inside uct_tensorrt.cpp's private namespace. Shares its exact DLL
// loader, CUDA error classification and logger; no second SDK search policy.
class NativeFormulaEngine final : public formula::Engine {
    using Clock=std::chrono::steady_clock;
    static void Require(bool value,const char* reason) { if(!value)throw std::runtime_error(reason); }
    static nvinfer1::Dims Dims(const QVector<qint64>& shape) {
        nvinfer1::Dims result{}; result.nbDims=int(shape.size());
        for(int i=0;i<result.nbDims;++i)result.d[i]=shape[i];
        return result;
    }
    static QVector<qint64> Shape(nvinfer1::Dims dims) {
        Require(dims.nbDims>0 && dims.nbDims<=4,"Invalid formula output rank.");
        QVector<qint64> result;
        for(int i=0;i<dims.nbDims;++i) {Require(dims.d[i]>=0 && dims.d[i]<=4096,"Unresolved formula output shape.");result.push_back(dims.d[i]);}
        return result;
    }
    static nvinfer1::DataType Type(const QByteArray& dtype) {
        if(dtype=="float32")return nvinfer1::DataType::kFLOAT;
        if(dtype=="int64")return nvinfer1::DataType::kINT64;
        if(dtype=="bool")return nvinfer1::DataType::kBOOL;
        throw std::runtime_error("Unsupported formula tensor type.");
    }
    struct Buffer {
        void* pointer{};qint64 capacity{};bool host{};
        ~Buffer() {if(pointer){if(host)cudaFreeHost(pointer);else cudaFree(pointer);}}
    };
    struct Allocator final : nvinfer1::IOutputAllocator {
        Buffer* buffer{};nvinfer1::Dims shape{};bool refused{};
        void* reallocateOutput(char const*,void*,uint64_t size,uint64_t alignment) noexcept override {
            if(!alignment || size>uint64_t(buffer->capacity) || reinterpret_cast<uintptr_t>(buffer->pointer)%alignment) {
                refused=true;return nullptr;
            }
            return buffer->pointer;
        }
        void* reallocateOutputAsync(char const* name,void* old,uint64_t size,uint64_t alignment,cudaStream_t) noexcept override {
            return reallocateOutput(name,old,size,alignment);
        }
        void notifyShape(char const*,nvinfer1::Dims const& value) noexcept override {shape=value;}
    };
    struct Build {
        QByteArray source;
        std::unique_ptr<nvinfer1::IBuilder> builder;
        std::unique_ptr<nvinfer1::INetworkDefinition> network;
        std::unique_ptr<nvonnxparser::IParser> parser;
        std::unique_ptr<nvinfer1::IBuilderConfig> config;
        std::unique_ptr<nvinfer1::IHostMemory> serialized;
    };
    struct BuildProgress final : nvinfer1::IProgressMonitor {
        const std::atomic<bool>& cancel;Clock::time_point deadline;
        BuildProgress(const std::atomic<bool>& c,Clock::time_point d):cancel(c),deadline(d){}
        void phaseStart(const char*,const char*,int32_t) noexcept override {}
        void phaseFinish(const char*) noexcept override {}
        bool stepComplete(const char*,int32_t) noexcept override {return !cancel.load() && Clock::now()<deadline;}
    };
    struct State {
        int device{};bool healthy=true;cudaStream_t stream{};qint64 allocated{};
        Logger logger;
        std::unique_ptr<Libraries> libraries;
        std::unique_ptr<BuildProgress> progress;
        std::vector<std::unique_ptr<Build>> builds;
        std::unique_ptr<nvinfer1::IRuntime> runtime;
        std::unique_ptr<nvinfer1::ICudaEngine> encoder,decoder;
        std::unique_ptr<nvinfer1::IExecutionContext> enc,dec;
        std::vector<std::unique_ptr<Buffer>> buffers;
        std::map<QByteArray,Buffer*> invariants,start;
        std::map<QByteArray,std::array<Buffer*,2>> bridges;
        std::map<QByteArray,std::unique_ptr<Allocator>> outputs[2];
        Buffer *image{},*condition{},*tokens{};
        QByteArray imageName;
        QVector<QByteArray> vocabulary;
        ~State() {
            // Only a synchronized healthy closure reaches here. Contexts and
            // allocators outlive every enqueue, then buffers and stream retire.
            dec.reset();enc.reset();outputs[0].clear();outputs[1].clear();
            buffers.clear();decoder.reset();encoder.reset();runtime.reset();
            builds.clear();progress.reset();libraries.reset();
            if(stream)cudaStreamDestroy(stream);
        }
        Buffer* Allocate(qint64 bytes,bool host=false) {
            bytes=std::max(qint64(256),bytes);
            Require(bytes<=formula::kBufferLimit && (!host || bytes<=32768) &&
                    allocated<=formula::kTotalBufferLimit-bytes,"Formula buffer budget exceeded.");
            auto buffer=std::make_unique<Buffer>();buffer->host=host;buffer->capacity=bytes;
            buffers.push_back(std::move(buffer));auto* result=buffers.back().get();
            if(host)Check(cudaHostAlloc(&result->pointer,size_t(bytes),cudaHostAllocDefault));
            else Check(cudaMalloc(&result->pointer,size_t(bytes)));
            allocated+=bytes;return result;
        }
    };
    formula::Definition model_;
    State* state_{};
    static void Retire(State* state) noexcept {
        if(!state)return;
        // No joins, including when a ready object is discarded by UI shutdown
        // or stale generation. Uncertain closures remain quarantined intact.
        try {
            AsyncCleanup::Start([state] {
                if(!state->healthy || cudaSetDevice(state->device)!=cudaSuccess ||
                    (state->stream && cudaStreamSynchronize(state->stream)!=cudaSuccess))return false;
                delete state;return true;
            });
        } catch(...) { /* retain exact state until process exit */ }
    }
    static void CheckTime(const std::atomic<bool>& cancel,Clock::time_point deadline) {
        Require(!cancel.load(),"Formula operation cancelled.");
        Require(Clock::now()<deadline,"Formula operation deadline expired.");
    }
    static void CheckCacheRoot(const QString& root) {
        Require(runtime_precision::LocalAbsolute(root),"Formula cache requires an absolute local directory.");
        for(QString path=QDir::cleanPath(root);;){
            const auto attributes=GetFileAttributesW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(path).utf16()));
            if(attributes!=INVALID_FILE_ATTRIBUTES)Require(!(attributes&FILE_ATTRIBUTE_REPARSE_POINT) &&
                (attributes&FILE_ATTRIBUTE_DIRECTORY),"Formula cache must not follow redirected paths.");
            else Require(GetLastError()==ERROR_FILE_NOT_FOUND || GetLastError()==ERROR_PATH_NOT_FOUND,"Formula cache is unavailable.");
            const auto parent=QFileInfo(path).absolutePath();if(parent==path)break;path=parent;
        }
        Require(QDir().mkpath(root),"Cannot create formula engine cache.");
    }
    QByteArray Plan(const QString& part,const QString& source,const QString& cache,const std::atomic<bool>& cancel,
                    Clock::time_point deadline,const QByteArray& uuid) {
        auto& s=*state_;
        const auto key=formula::CacheKey(model_,part,uuid,s.libraries->tensorRtVersion);
        const auto target=QDir(cache).filePath(QString::fromLatin1(key)+".formula-plan");
        const QFileInfo info(target);
        if(info.exists() || info.isSymLink()) {
            Require(info.isFile() && !info.isSymLink() && info.size()>32 && info.size()<formula::kPlanLimit,
                    "Formula cached plan is outside its bound.");
            QFile file(target);Require(file.open(QIODevice::ReadOnly),"Cannot read formula cached plan.");
            const auto bytes=file.read(formula::kPlanLimit);
            Require(file.error()==QFileDevice::NoError && file.atEnd() &&
                QCryptographicHash::hash(bytes.sliced(32),QCryptographicHash::Sha256)==bytes.first(32),"Formula cached plan checksum mismatch.");
            CheckTime(cancel,deadline);return bytes.sliced(32);
        }
        s.builds.push_back(std::make_unique<Build>());auto& b=*s.builds.back();
        b.source=formula::ReadPinned(model_,source,part+".onnx",cancel);
        CheckTime(cancel,deadline);
        const auto sdk=runtime_paths::TensorRtRoot(qEnvironmentVariable("OKUFLOW_UCT_TENSORRT_ROOT"),
            QCoreApplication::applicationDirPath(),qEnvironmentVariable("LOCALAPPDATA"));
        s.libraries->PrepareBuilder(sdk);
        b.builder.reset(static_cast<nvinfer1::IBuilder*>(s.libraries->builder(&s.logger,NV_TENSORRT_VERSION)));
        Require(bool(b.builder),"Formula builder creation failed.");
        b.network.reset(b.builder->createNetworkV2(0));Require(bool(b.network),"Formula network creation failed.");
        b.parser.reset(static_cast<nvonnxparser::IParser*>(s.libraries->createParser(b.network.get(),&s.logger,NV_ONNX_PARSER_VERSION)));
        Require(b.parser && b.parser->parse(b.source.constData(),b.source.size()),"Formula source parsing failed.");
        b.config.reset(b.builder->createBuilderConfig());Require(bool(b.config),"Formula builder configuration failed.");
        b.config->setFlag(nvinfer1::BuilderFlag::kTF32);
        b.config->setBuilderOptimizationLevel(2);b.config->setMaxAuxStreams(0);
        b.config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE,512ull*1024*1024);
        b.config->setProgressMonitor(s.progress.get());b.config->setProfileStream(s.stream);
        auto* profile=b.builder->createOptimizationProfile();Require(profile,"Formula profile creation failed.");
        for(int i=0;i<b.network->getNbInputs();++i) {
            const auto* input=b.network->getInput(i);const QByteArray name=input->getName();
            formula::Profile p;
            if(part=="encoder") {p.minimum=p.optimum=p.maximum={1,1,768,768};Require(b.network->getNbInputs()==1,"Unexpected formula encoder input.");}
            else {
                bool found=false;
                for(const auto& q:model_.states+model_.invariants)if(q.input==name){p=q;found=true;break;}
                Require(found && input->getType()==Type(p.dtype),"Unexpected formula decoder input.");
            }
            Require(profile->setDimensions(name.constData(),nvinfer1::OptProfileSelector::kMIN,Dims(p.minimum)) &&
                profile->setDimensions(name.constData(),nvinfer1::OptProfileSelector::kOPT,Dims(p.optimum)) &&
                profile->setDimensions(name.constData(),nvinfer1::OptProfileSelector::kMAX,Dims(p.maximum)),"Formula shape profile refused.");
        }
        Require(b.config->addOptimizationProfile(profile)>=0,"Formula optimization profile refused.");
        CheckTime(cancel,deadline);
        b.serialized.reset(b.builder->buildSerializedNetwork(*b.network,*b.config));
        Check(cudaPeekAtLastError());CheckTime(cancel,deadline);
        Require(b.serialized && b.serialized->size()>0 && b.serialized->size()<size_t(formula::kPlanLimit-32),"Formula native build failed or exceeded its bound.");
        Check(cudaStreamSynchronize(s.stream));
        const QByteArray plan(static_cast<const char*>(b.serialized->data()),qsizetype(b.serialized->size()));
        QSaveFile file(target);file.setDirectWriteFallback(false);
        Require(file.open(QIODevice::WriteOnly) && file.write(QCryptographicHash::hash(plan,QCryptographicHash::Sha256))==32 &&
            file.write(plan)==plan.size(),"Cannot write formula cached plan.");
        CheckTime(cancel,deadline);Require(file.commit(),"Cannot commit formula cached plan.");
        return plan;
    }
public:
    explicit NativeFormulaEngine(formula::Definition model):model_(std::move(model)),state_(new State){}
    ~NativeFormulaEngine() override {Retire(std::exchange(state_,nullptr));}
    void Prepare(const QString& source,const QString& cache,const std::atomic<bool>& cancel,Clock::time_point deadline) {
        auto& s=*state_;
        try {
            CheckTime(cancel,deadline);CheckCacheRoot(cache);
            QLockFile lock(QDir(cache).filePath(".formula-setup.lock"));lock.setStaleLockTime(0);
            Require(lock.tryLock(0),"Another formula setup is active.");
            Check(cudaGetDevice(&s.device));cudaDeviceProp device{};Check(cudaGetDeviceProperties(&device,s.device));
            Require(device.major>=8,"TF32 Math requires an Ampere or newer NVIDIA GPU.");
            Check(cudaStreamCreateWithFlags(&s.stream,cudaStreamNonBlocking));
            const auto sdk=runtime_paths::TensorRtRoot(qEnvironmentVariable("OKUFLOW_UCT_TENSORRT_ROOT"),
                QCoreApplication::applicationDirPath(),qEnvironmentVariable("LOCALAPPDATA"));
            s.libraries=std::make_unique<Libraries>(sdk);
            s.progress=std::make_unique<BuildProgress>(cancel,deadline);
            s.vocabulary=formula::LoadVocabulary(formula::ReadPinned(model_,source,"config.json",cancel));
            s.runtime.reset(static_cast<nvinfer1::IRuntime*>(s.libraries->runtime(&s.logger,NV_TENSORRT_VERSION)));
            Require(bool(s.runtime),"Formula runtime creation failed.");
            const QByteArray uuid(device.uuid.bytes,16);
            const auto encoder=Plan("encoder",source,cache,cancel,deadline,uuid);
            s.encoder.reset(s.runtime->deserializeCudaEngine(encoder.constData(),encoder.size()));
            Require(bool(s.encoder),"Formula encoder plan refused.");
            const auto decoder=Plan("decoder",source,cache,cancel,deadline,uuid);
            s.decoder.reset(s.runtime->deserializeCudaEngine(decoder.constData(),decoder.size()));
            Require(bool(s.decoder),"Formula decoder plan refused.");
            Require(s.encoder->getDeviceMemorySizeV2()>=0 && s.encoder->getDeviceMemorySizeV2()<=1024ll*1024*1024 &&
                s.decoder->getDeviceMemorySizeV2()>=0 && s.decoder->getDeviceMemorySizeV2()<=1024ll*1024*1024,
                "Formula context memory exceeds its bound.");
            s.enc.reset(s.encoder->createExecutionContext());s.dec.reset(s.decoder->createExecutionContext());
            Require(s.enc && s.dec,"Formula execution context creation failed.");
            Require(s.encoder->getNbIOTensors()==1+model_.invariants.size(),"Unexpected formula encoder outputs.");
            for(int i=0;i<s.encoder->getNbIOTensors();++i) {
                const QByteArray n=s.encoder->getIOTensorName(i);
                Require(s.encoder->getTensorLocation(n.constData())==nvinfer1::TensorLocation::kDEVICE &&
                    s.encoder->getTensorDataType(n.constData())==nvinfer1::DataType::kFLOAT,"Unexpected formula encoder ABI.");
                if(s.encoder->getTensorIOMode(n.constData())==nvinfer1::TensorIOMode::kINPUT) {
                    s.imageName=n;Require(Shape(s.encoder->getTensorShape(n.constData()))==QVector<qint64>({1,1,768,768}),"Unexpected formula image shape.");
                    s.image=s.Allocate(768*768*4);Require(s.enc->setTensorAddress(n.constData(),s.image->pointer),"Cannot bind formula image.");
                } else {
                    const auto p=std::find_if(model_.invariants.begin(),model_.invariants.end(),[&](const auto& q){return q.input==n;});
                    Require(p!=model_.invariants.end(),"Unexpected formula invariant output.");
                    const auto shape=Shape(s.enc->getTensorShape(n.constData()));Require(formula::ShapeWithin(shape,*p),"Invalid formula invariant shape.");
                    auto* b=s.Allocate(formula::TensorBytes(shape,p->dtype));s.invariants[n]=b;
                    Require(s.enc->setTensorAddress(n.constData(),b->pointer),"Cannot bind formula invariant.");
                }
            }
            Require(s.image,"Formula image input missing.");
            const auto outputs=model_.states.size()+1;
            Require(s.decoder->getNbIOTensors()==model_.states.size()+model_.invariants.size()+outputs,"Unexpected formula decoder tensor count.");
            for(const auto& p:model_.states+model_.invariants) {
                Require(s.decoder->getTensorIOMode(p.input.constData())==nvinfer1::TensorIOMode::kINPUT &&
                    s.decoder->getTensorDataType(p.input.constData())==Type(p.dtype),"Unexpected formula decoder input ABI.");
                const bool host=s.decoder->getTensorLocation(p.input.constData())==nvinfer1::TensorLocation::kHOST;
                if(s.invariants.contains(p.input))Require(!host,"Formula invariant must remain on device.");
                else {Require(!host || formula::TensorBytes(p.maximum,p.dtype)<=256,"Host formula cache is forbidden.");
                    s.start[p.input]=s.Allocate(formula::TensorBytes(p.maximum,p.dtype),host);}
            }
            for(int i=0;i<s.decoder->getNbIOTensors();++i) {
                const QByteArray n=s.decoder->getIOTensorName(i);
                if(s.decoder->getTensorIOMode(n.constData())!=nvinfer1::TensorIOMode::kOUTPUT)continue;
                const auto p=std::find_if(model_.states.begin(),model_.states.end(),[&](const auto& q){return q.output==n;});
                QVector<qint64> shape;QByteArray dtype;
                if(p==model_.states.end()) {Require(n==model_.condition,"Unexpected formula output.");shape={1};dtype="bool";}
                else {shape=p->maximum;dtype=p->dtype;for(qsizetype axis=0;axis<shape.size();++axis)if(shape[axis]>128 && (axis==1 || axis==2))shape[axis]+=16;}
                Require(s.decoder->getTensorDataType(n.constData())==Type(dtype),"Unexpected formula output type.");
                const bool host=s.decoder->getTensorLocation(n.constData())==nvinfer1::TensorLocation::kHOST;
                const auto capacity=formula::TensorBytes(shape,dtype)+1024;
                Require(!host || capacity<=4096,"Host formula cache output is forbidden.");
                for(int bank=0;bank<2;++bank) {
                    auto allocator=std::make_unique<Allocator>();allocator->buffer=s.Allocate(capacity,host);
                    s.outputs[bank][n]=std::move(allocator);
                }
                if(p!=model_.states.end() && host!=s.start[p->input]->host) {
                    Require(formula::TensorBytes(p->maximum,p->dtype)<=256,"Formula cache readback is forbidden.");
                    for(int bank=0;bank<2;++bank)s.bridges[p->input][bank]=s.Allocate(formula::TensorBytes(p->maximum,p->dtype),!host);
                }
            }
            s.condition=s.Allocate(256,true);s.tokens=s.Allocate((model_.maxTokens+8)*8,true);
            Check(cudaStreamSynchronize(s.stream));CheckTime(cancel,deadline);
            s.builds.clear();s.progress.reset(); // confirmed completion; release parser snapshots and cancellation reference
        } catch(const CudaFailure&) {s.healthy=false;throw;}
    }
    formula::Result Recognize(std::span<const float> tensor,const std::atomic<bool>& cancel,Clock::time_point deadline,int tokenLimit) override {
        formula::Result result;auto& s=*state_;const auto started=Clock::now();
        if(tokenLimit==0)tokenLimit=model_.maxTokens;
        bool submitted=false;
        try {
            Require(s.healthy,"Formula engine is quarantined.");CheckTime(cancel,deadline);
            Require(tokenLimit>=1 && tokenLimit<=model_.maxTokens && tensor.size()==768*768 &&
                std::all_of(tensor.begin(),tensor.end(),[](float v){return std::isfinite(v) && v>=-16 && v<=16;}),"Invalid formula input tensor or token limit.");
            Check(cudaSetDevice(s.device));submitted=true;
            Check(cudaMemcpyAsync(s.image->pointer,tensor.data(),tensor.size_bytes(),cudaMemcpyHostToDevice,s.stream));
            Require(s.enc->enqueueV3(s.stream),"Formula encoder inference failed.");
            struct Binding {Buffer* buffer;QVector<qint64> shape;};std::map<QByteArray,Binding> states;
            for(const auto& p:model_.states) {
                auto* b=s.start.at(p.input);
                if(!p.initialData.isEmpty()) {
                    if(b->host)std::memcpy(b->pointer,p.initialData.constData(),p.initialData.size());
                    else Check(cudaMemcpyAsync(b->pointer,p.initialData.constData(),p.initialData.size(),cudaMemcpyHostToDevice,s.stream));
                }
                states[p.input]={b,p.initial};
            }
            for(const auto& p:model_.invariants)Require(s.dec->setInputShape(p.input.constData(),Dims(p.maximum)) &&
                s.dec->setTensorAddress(p.input.constData(),s.invariants.at(p.input)->pointer),"Cannot bind formula invariant input.");
            for(int iteration=0;iteration<tokenLimit;++iteration) {
                if(cancel.load() || Clock::now()>=deadline) {
                    Check(cudaStreamSynchronize(s.stream));submitted=false;CheckTime(cancel,deadline);
                }
                const int bank=iteration%2;
                for(const auto& [name,value]:states)Require(s.dec->setInputShape(name.constData(),Dims(value.shape)) &&
                    s.dec->setTensorAddress(name.constData(),value.buffer->pointer),"Cannot bind formula recurrent state.");
                for(auto& [name,a]:s.outputs[bank]) {
                    a->refused=false;a->shape.nbDims=-1;
                    Require(s.dec->setOutputAllocator(name.constData(),a.get()) && s.dec->setTensorAddress(name.constData(),a->buffer->pointer),"Cannot bind formula output allocator.");
                }
                submitted=true;Require(s.dec->enqueueV3(s.stream),"Formula decoder inference failed.");
                auto* condition=s.outputs[bank].at(model_.condition)->buffer;
                if(!condition->host)Check(cudaMemcpyAsync(s.condition->pointer,condition->pointer,1,cudaMemcpyDeviceToHost,s.stream));
                Check(cudaStreamSynchronize(s.stream));submitted=false;
                for(const auto& [name,a]:s.outputs[bank])Require(!a->refused,"Formula output exceeded its allocation bound.");
                const bool keepGoing=*static_cast<const unsigned char*>(condition->host?condition->pointer:s.condition->pointer)!=0;
                const auto outputShape=[&](const QByteArray& n) {
                    const auto& a=*s.outputs[bank].at(n);return Shape(a.shape.nbDims<0?s.dec->getTensorShape(n.constData()):a.shape);
                };
                result.iterations=iteration+1;CheckTime(cancel,deadline);
                if(!keepGoing) {
                    const auto shape=outputShape(model_.tokens);
                    Require(shape.size()==2 && shape[0]==1 && shape[1]>0 && shape[1]<=model_.maxTokens+8,"Invalid formula token output shape.");
                    const auto bytes=formula::TensorBytes(shape,"int64");
                    auto* source=s.outputs[bank].at(model_.tokens)->buffer;
                    Require(!source->host && bytes<=s.tokens->capacity,"Invalid formula token output location.");
                    submitted=true;Check(cudaMemcpyAsync(s.tokens->pointer,source->pointer,size_t(bytes),cudaMemcpyDeviceToHost,s.stream));
                    Check(cudaStreamSynchronize(s.stream));submitted=false;CheckTime(cancel,deadline);
                    const auto* ids=static_cast<const qint64*>(s.tokens->pointer);
                    result.tokens=QVector<qint64>(ids,ids+shape[1]);
                    result.latex=formula::DecodeTokens(result.tokens,s.vocabulary);result.ok=true;break;
                }
                states.clear();
                for(const auto& p:model_.states) {
                    auto shape=outputShape(p.output);Require(formula::ShapeWithin(shape,p),"Formula recurrent state escaped its profile.");
                    auto* buffer=s.outputs[bank].at(p.output)->buffer;
                    if(s.bridges.contains(p.input)) {
                        auto* next=s.bridges.at(p.input)[bank];const auto bytes=formula::TensorBytes(shape,p.dtype);
                        submitted=true;Check(cudaMemcpyAsync(next->pointer,buffer->pointer,size_t(bytes),cudaMemcpyDefault,s.stream));buffer=next;
                    }
                    states[p.input]={buffer,shape};
                }
                if(submitted) {Check(cudaStreamSynchronize(s.stream));submitted=false;}
            }
            if(!result.ok)result.code="formula_token_limit";
        } catch(const CudaFailure& error) {s.healthy=false;result.code="formula_cuda_fault";result.detail=QString::fromUtf8(error.what());}
          catch(const std::exception& error) {
            if(submitted)s.healthy=false;
            result.cancelled=cancel.load();result.code=result.cancelled?"cancelled":Clock::now()>=deadline?"formula_deadline":"formula_refused";
            result.detail=QString::fromUtf8(error.what());
        }
        result.milliseconds=std::chrono::duration<double,std::milli>(Clock::now()-started).count();return result;
    }
};
