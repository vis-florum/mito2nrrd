#include <tiffio.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <zlib.h>
#include <omp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;
using Bytes = std::vector<unsigned char>;
using Tiff = std::unique_ptr<TIFF, decltype(&TIFFClose)>;
static void require(bool ok, const std::string& why) { if (!ok) throw std::runtime_error(why); }
static Tiff open_tiff(const fs::path& path) {
    // Disable libtiff's default whole-file mmap: each decoder would otherwise
    // retain resident input mappings, defeating the bounded-memory pipeline.
    Tiff t(TIFFOpen(path.c_str(), "rm"), TIFFClose);
    require(bool(t), "Cannot open TIFF: " + path.string()); return t;
}
struct Page {
    uint64_t offset; uint32_t w, h; uint16_t bits, format, samples;
    uint16_t a=0, b=0; bool numbered=false;
};
static Page page(TIFF* t) {
    Page p{}; p.offset=TIFFCurrentDirOffset(t);
    require(TIFFGetField(t, TIFFTAG_IMAGEWIDTH, &p.w) && TIFFGetField(t, TIFFTAG_IMAGELENGTH, &p.h), "Missing image dimensions");
    TIFFGetFieldDefaulted(t, TIFFTAG_BITSPERSAMPLE, &p.bits);
    TIFFGetFieldDefaulted(t, TIFFTAG_SAMPLEFORMAT, &p.format);
    TIFFGetFieldDefaulted(t, TIFFTAG_SAMPLESPERPIXEL, &p.samples);
    p.numbered=TIFFGetField(t, TIFFTAG_PAGENUMBER, &p.a, &p.b);
    return p;
}
static size_t product(size_t a, size_t b) {
    require(b==0 || a<=std::numeric_limits<size_t>::max()/b, "Image size overflow"); return a*b;
}
// Decode complete codec blocks, copying only the requested rows. libtiff returns native-endian samples.
static void read_rows(TIFF* t, const Page& p, uint32_t first, uint32_t count, unsigned char* dst, Bytes& scratch) {
    require(first<=p.h && count<=p.h-first, "Row range outside image");
    size_t pixel=p.bits/8, row=product(p.w,pixel);
    if (TIFFIsTiled(t)) {
        uint32_t tw=0, th=0;
        require(TIFFGetField(t,TIFFTAG_TILEWIDTH,&tw) && TIFFGetField(t,TIFFTAG_TILELENGTH,&th) && tw && th, "Invalid tile dimensions");
        auto size=TIFFTileSize(t); require(size>0,"Invalid tile size"); scratch.resize(size);
        size_t stride=product(tw,pixel);
        for (uint64_t y=first/th*th; y<uint64_t(first)+count; y+=th)
            for (uint64_t x=0; x<p.w; x+=tw) {
                auto got=TIFFReadEncodedTile(t,TIFFComputeTile(t,x,y,0,0),scratch.data(),size);
                uint32_t lo=std::max<uint64_t>(first,y), hi=std::min<uint64_t>(uint64_t(first)+count,y+th);
                size_t n=product(std::min<uint64_t>(tw,p.w-x),pixel);
                require(got>=0 && size_t(got)>=product(hi-y-1,stride)+n, "Truncated/invalid TIFF tile");
                for (uint32_t r=lo;r<hi;++r) std::memcpy(dst+product(r-first,row)+x*pixel,scratch.data()+product(r-y,stride),n);
            }
    } else {
        uint32_t rows=0; TIFFGetFieldDefaulted(t,TIFFTAG_ROWSPERSTRIP,&rows); require(rows>0,"Invalid rows per strip");
        auto size=TIFFStripSize(t); require(size>0,"Invalid strip size"); scratch.resize(size);
        for (uint64_t y=uint64_t(first/rows)*rows; y<uint64_t(first)+count; y+=rows) {
            auto got=TIFFReadEncodedStrip(t,TIFFComputeStrip(t,y,0),scratch.data(),size);
            uint32_t lo=std::max<uint64_t>(first,y), hi=std::min<uint64_t>(uint64_t(first)+count,y+rows);
            require(got>=0 && size_t(got)>=product(hi-y,row),"Truncated/invalid TIFF strip");
            std::memcpy(dst+product(lo-first,row),scratch.data()+product(lo-y,row),product(hi-lo,row));
        }
    }
}
static std::string normalize_xml(std::string s) {
    // Microtec uses spaces and square brackets in element names, which are invalid XML.
    // Match the legacy sidecar's space substitution; additionally accept logInfo names with brackets.
    std::replace(s.begin(),s.end(),' ','_');
    s.erase(std::remove_if(s.begin(),s.end(),[](char c){return c=='[' || c==']';}),s.end());
    while (!s.empty() && s.back()=='\0') s.pop_back();
    return s;
}
static xmlNode* child(xmlNode* n, const char* name) {
    for (auto p=n?n->children:nullptr;p;p=p->next)
        if (p->type==XML_ELEMENT_NODE && xmlStrEqual(p->name,BAD_CAST name)) return p;
    throw std::runtime_error(std::string("Missing metadata element: ")+name);
}
static std::array<double,3> spacing(const std::string& s) {
    require(s.size()>12 && s.size()<=size_t(std::numeric_limits<int>::max()), "Missing scanning metadata; possible sinogram");
    auto doc=std::unique_ptr<xmlDoc,decltype(&xmlFreeDoc)>(xmlReadMemory(s.data(),s.size(),"metadata.xml",nullptr,XML_PARSE_NONET|XML_PARSE_NOERROR|XML_PARSE_NOWARNING),xmlFreeDoc);
    require(bool(doc),"Invalid scanning XML");
    require(!doc->intSubset && !doc->extSubset,"Metadata DTDs are unsupported");
    auto root=xmlDocGetRootElement(doc.get()); require(root,"Empty scanning XML");
    std::array<const char*,3> names; xmlNode* volume;
    if (xmlStrEqual(root->name,BAD_CAST "CT")) {volume=child(root,"Volume"); names={"Voxel_Dim_X","Voxel_Dim_Y","Voxel_Dim_Z"};}
    else if (xmlStrEqual(root->name,BAD_CAST "logInfo")) {volume=child(root,"dimVox");names={"dimVox0","dimVox1","dimVox2"};}
    else throw std::runtime_error("Unsupported scanning metadata root");
    std::array<double,3> result;
    for (size_t i=0;i<3;++i) {
        auto content=std::unique_ptr<xmlChar,decltype(xmlFree)>(xmlNodeGetContent(child(volume,names[i])),xmlFree);
        require(bool(content),"Empty voxel spacing"); std::string value(reinterpret_cast<char*>(content.get()));
        size_t used=0; result[i]=std::stod(value,&used);
        require(value.find_first_not_of("\r\n\t",used)==std::string::npos && std::isfinite(result[i]) && result[i]>0,"Invalid voxel spacing");
    }
    return result;
}
static std::string type(const Page& p) {
    require(p.samples==1,"Only scalar grayscale TIFF is supported");
    if (p.format==SAMPLEFORMAT_IEEEFP) {if(p.bits==32)return "float"; if(p.bits==64)return "double";}
    if (p.format==SAMPLEFORMAT_UINT || p.format==SAMPLEFORMAT_INT) {
        std::string prefix=p.format==SAMPLEFORMAT_INT?"int":"uint";
        if(p.bits==8 || p.bits==16 || p.bits==32 || p.bits==64) return prefix+std::to_string(p.bits);
    }
    throw std::runtime_error("Unsupported TIFF sample type");
}
template<class T> static void transpose(const unsigned char* src, unsigned char* dst, size_t h, size_t w) {
    // memcpy avoids alignment/aliasing assumptions about byte buffers. Tiles keep accesses in cache.
    for(size_t y0=0;y0<h;y0+=32) for(size_t x0=0;x0<w;x0+=32)
        for(size_t x=x0;x<std::min(w,x0+32);++x) for(size_t y=y0;y<std::min(h,y0+32);++y)
            std::memcpy(dst+(x*h+y)*sizeof(T),src+(y*w+x)*sizeof(T),sizeof(T));
}
static void transpose(const Bytes& src, unsigned char* dst, size_t h,size_t w,size_t bytes) {
    switch(bytes) {
    case 1:transpose<uint8_t>(src.data(),dst,h,w);break;
    case 2:transpose<uint16_t>(src.data(),dst,h,w);break;
    case 4:transpose<uint32_t>(src.data(),dst,h,w);break;
    case 8:transpose<uint64_t>(src.data(),dst,h,w);break;
    default:throw std::runtime_error("Unsupported sample width");
    }
}
class Output {
    std::ofstream file; z_stream z{}; Bytes buffer; bool gzip;
public:
    Output(const fs::path& path,const std::string& header,int level):file(path,std::ios::binary),gzip(level>=0) {
        require(bool(file),"Cannot create output");file.write(header.data(),header.size());
        if(gzip) { require(deflateInit2(&z,level,Z_DEFLATED,15+16,8,Z_DEFAULT_STRATEGY)==Z_OK,"Cannot initialize gzip");buffer.resize(1<<20); }
    }
    ~Output(){if(gzip)deflateEnd(&z);}
    void write(const unsigned char* p,size_t n) {
        if(!gzip) {file.write(reinterpret_cast<const char*>(p),n);require(bool(file),"Output write failed");return;}
        while(n) {size_t chunk=std::min<size_t>(n,1<<30);z.next_in=const_cast<Bytef*>(p);z.avail_in=chunk;
            do {z.next_out=buffer.data();z.avail_out=buffer.size();require(deflate(&z,Z_NO_FLUSH)==Z_OK,"Gzip failed");
                file.write(reinterpret_cast<char*>(buffer.data()),buffer.size()-z.avail_out);
            } while(z.avail_in || z.avail_out==0);
            p+=chunk;n-=chunk;
        }
        require(bool(file),"Output write failed");
    }
    void finish() {
        if(gzip) {int status;do {z.next_in=nullptr;z.avail_in=0;z.next_out=buffer.data();z.avail_out=buffer.size();status=deflate(&z,Z_FINISH);
            require(status==Z_OK || status==Z_STREAM_END,"Gzip finalization failed");file.write(reinterpret_cast<char*>(buffer.data()),buffer.size()-z.avail_out);
        }while(status!=Z_STREAM_END);}
        file.flush();require(bool(file),"Output flush failed");file.close();require(!file.fail(),"Output close failed");
    }
};
struct Options {int workers=6,threads=1,level=-1;size_t batch_mb=64;fs::path outdir;bool force=false;std::vector<fs::path> files;};
static std::mutex logging;
static void convert(const fs::path& path,const Options& o) {
    auto start=std::chrono::steady_clock::now();auto t=open_tiff(path);std::vector<Page> all;
    do {all.push_back(page(t.get()));}while(TIFFReadDirectory(t.get()));
    require(all.size()>=2,"Expected volume and metadata TIFF pages");
    Page meta=all.back();require(meta.bits==8 && meta.samples==1 && meta.h==1 && meta.w<=16*1024*1024,"Expected final page to contain scanning XML");
    require(TIFFSetSubDirectory(t.get(),meta.offset),"Cannot select metadata page");
    Bytes metadata(meta.w),scratch;read_rows(t.get(),meta,0,1,metadata.data(),scratch);
    std::string xml=normalize_xml(std::string(metadata.begin(),metadata.end()));auto sp=spacing(xml);
    Page first=all.front();std::string dtype=type(first);std::vector<Page> images;
    bool auxiliary=false;
    for(size_t i=0;i+1<all.size();++i) {
        const auto& p=all[i];
        if(p.w==first.w && p.h==first.h && p.bits==first.bits && p.format==first.format && p.samples==first.samples) {
            require(!auxiliary,"Volume pages interleaved with auxiliary pages are unsupported");images.push_back(p);
        } else auxiliary=true; // Trajectory/other series after the volume are not voxel data.
    }
    size_t h=first.h,w=first.w,k=images.size();bool flat=images.size()==1;
    if(flat) {
        require(first.numbered,"Single-page volume needs PageNumber=(slice rows,slice count) or (0,1)");
        if(first.a==0 && first.b==1) { k=1; }
        else {require(first.a && first.b,"Invalid flattened PageNumber");h=first.a;k=first.b;require(product(h,k)==first.h,"Flattened TIFF height disagrees with PageNumber");}
    }
    size_t bytes=first.bits/8,slice=product(product(h,w),bytes);product(slice,k);
    require(TIFFSetSubDirectory(t.get(),first.offset),"Cannot select volume page");
    // One enormous compressed strip cannot be independently decoded by slice.
    // Read its scanlines in sequence instead of repeatedly inflating the whole strip.
    bool sequential=flat && !TIFFIsTiled(t.get()) && uint64_t(TIFFStripSize(t.get()))>slice;
    fs::path base=o.outdir.empty()?path.parent_path()/path.stem():o.outdir/path.stem();
    fs::path nrrd=base.string()+".nrrd", settings=base.string()+"_ScanSettings.xml";
    require(o.force || (!fs::exists(nrrd) && !fs::exists(settings)),"Output exists; use --force: "+base.string());
    fs::path temp=nrrd.string()+".tmp."+std::to_string(getpid()), xtemp=settings.string()+".tmp."+std::to_string(getpid());
    try {
        uint16_t one=1;bool little=*reinterpret_cast<unsigned char*>(&one)==1;
        std::ostringstream header;header.precision(17);
        header<<"NRRD0005\n# Converted by mito_unpack\ntype: "<<dtype<<"\ndimension: 3\nsizes: "<<h<<' '<<w<<' '<<k<<"\nencoding: "<<(o.level<0?"raw":"gzip")<<'\n';
        if(bytes>1)header<<"endian: "<<(little?"little":"big")<<'\n';
        header<<"spacings: "<<sp[0]<<' '<<sp[1]<<' '<<sp[2]<<"\nunits: \"mm\" \"mm\" \"mm\"\n\n";
        Output output(temp,header.str(),o.level);
        int threads=sequential?1:std::min<size_t>(o.threads,k);std::vector<Tiff> handles;std::vector<Bytes> inputs(threads),blocks(threads);
        for(int i=0;i<threads;++i) {handles.push_back(open_tiff(path));inputs[i].resize(slice);}
        size_t batch=std::max<size_t>(1,product(o.batch_mb,1024*1024)/slice);batch=std::min(k,batch);Bytes result(product(batch,slice));
        std::string failure;
        for(size_t begin=0;begin<k;begin+=batch) {
            size_t count=std::min(batch,k-begin);
            #pragma omp parallel for if(threads>1) num_threads(threads) schedule(static)
            for(size_t i=0;i<count;++i) {
                int id=omp_get_thread_num();auto handle=handles[id].get();size_t index=begin+i;auto p=flat?first:images[index];
                try {
                    if(TIFFCurrentDirOffset(handle)!=p.offset)
                        require(TIFFSetSubDirectory(handle,p.offset),"Cannot select image page");
                    if(sequential) {
                        for(size_t row=0;row<h;++row)
                            require(TIFFReadScanline(handle,inputs[id].data()+row*w*bytes,index*h+row,0)>=0,"Invalid TIFF scanline");
                    } else read_rows(handle,p,flat?index*h:0,h,inputs[id].data(),blocks[id]);
                    transpose(inputs[id],result.data()+i*slice,h,w,bytes);
                }catch(const std::exception& e) {
                    #pragma omp critical(mito_error)
                    {if(failure.empty())failure=e.what();}
                }
            }
            require(failure.empty(),failure);output.write(result.data(),product(count,slice));
        }
        output.finish();std::ofstream x(xtemp,std::ios::binary);x.write(xml.data(),xml.size());x.close();require(!x.fail(),"Metadata write failed");
        fs::rename(xtemp,settings);fs::rename(temp,nrrd);
    }catch(...) {std::error_code ec;fs::remove(temp,ec);fs::remove(xtemp,ec);throw;}
    double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    std::lock_guard<std::mutex> lock(logging);std::cout<<path<<": "<<h<<'x'<<w<<'x'<<k<<" "<<dtype<<", "<<seconds<<" s -> "<<nrrd<<std::endl;
}
static int positive(const std::string& s) {size_t used=0;int v=std::stoi(s,&used);require(used==s.size() && v>0,"Expected a positive integer: "+s);return v;}
int main(int argc,char** argv) {
    try {
        Options o;
        for(int i=1;i<argc;++i) {
            std::string arg=argv[i];auto value=[&](){require(i+1<argc,"Missing value for "+arg);return std::string(argv[++i]);};
            if(arg=="--workers")o.workers=positive(value());else if(arg=="--threads")o.threads=positive(value());
            else if(arg=="--batch-mb")o.batch_mb=positive(value());else if(arg=="--output-dir")o.outdir=value();
            else if(arg=="--force")o.force=true;else if(arg=="--compress")o.level=6;
            else if(arg=="--compression-level") {auto s=value();size_t used=0;o.level=std::stoi(s,&used);require(used==s.size() && o.level>=0 && o.level<=9,"Compression level must be 0..9");}
            else if(arg=="--help" || arg=="-h") {std::cout<<"Usage: mito_unpack [options] file.tiff ...\n  --workers N           Concurrent files (default 6)\n  --threads N           Decode threads per file (default 1)\n  --batch-mb N          Output buffer MiB per file (default 64)\n  --output-dir DIR      Destination directory (default alongside input)\n  --compress            Gzip level 6 (default raw)\n  --compression-level N Gzip level 0..9\n  --force               Replace existing outputs\n";return 0;}
            else {require(arg.empty() || arg[0]!='-',"Unknown option: "+arg);o.files.emplace_back(arg);}
        }
        require(!o.files.empty(),"No input files; use --help");if(!o.outdir.empty())fs::create_directories(o.outdir);
        // Prevent parallel workers from targeting the same output (including aliases).
        std::vector<fs::path> targets;
        for(const auto& f:o.files) {auto base=o.outdir.empty()?f.parent_path()/f.stem():o.outdir/f.stem();auto target=fs::weakly_canonical(base.string()+".nrrd");
            require(std::find(targets.begin(),targets.end(),target)==targets.end(),"Duplicate output destination: "+target.string());targets.push_back(target);}
        xmlInitParser();std::atomic<size_t> next{0};std::atomic<int> failures{0};std::vector<std::thread> pool;
        for(size_t j=0;j<std::min<size_t>(o.workers,o.files.size());++j)pool.emplace_back([&](){for(;;){size_t i=next++;if(i>=o.files.size())break;
            try {convert(o.files[i],o);}catch(const std::exception& e){++failures;std::lock_guard<std::mutex> lock(logging);std::cerr<<o.files[i]<<": ERROR: "<<e.what()<<std::endl;}}});
        for(auto& worker:pool)worker.join();
        return failures?1:0;
    }catch(const std::exception& e){std::cerr<<"ERROR: "<<e.what()<<'\n';return 1;}
}
