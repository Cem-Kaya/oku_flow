// First-party bounded source-surface geometry, shared by CPU and CUDA workers.
#pragma once
#include <cmath>
#if defined(__CUDACC__)
#define OKUFLOW_SURFACE_HD __host__ __device__
#else
#define OKUFLOW_SURFACE_HD
#endif
namespace okuflow::uct::surface_geometry {
inline constexpr int Columns=96, Rows=40, Cells=Columns*Rows, WordLimit=128;
struct Point { double x{},y{}; };
struct Rgb { int r{},g{},b{}; };
struct Scratch {
    int state[Cells];
    // Two queues keep the best component's cells while later seeds flood.
    unsigned short queue[2][Cells];
    int left[Rows],right[Rows],top[Columns],bottom[Columns];
    int size;
};
struct Result { Point quad[4]; bool fitted{}; int stage{}; };
struct Line { double a{},b{}; bool valid{}; };
OKUFLOW_SURFACE_HD inline double Abs(double x){return x<0?-x:x;}
OKUFLOW_SURFACE_HD inline int Min(int a,int b){return a<b?a:b;}
OKUFLOW_SURFACE_HD inline int Max(int a,int b){return a>b?a:b;}
OKUFLOW_SURFACE_HD inline bool Finite(double x){return x==x && Abs(x)<1e100;}
// A team executes one request. CPU workers use this one-member team; CUDA
// supplies a block team with the same operations. Every decision below runs
// on identical values either way: cross-member combinations are integer
// counts, minima/maxima whose ties keep the first sequential occurrence, or
// sums of quarter-integers far below 2^53, so the order of combination
// cannot change a single bit.
struct SerialTeam {
    int Rank()const{return 0;}
    int Size()const{return 1;}
    void Sync()const{}
    template<class T,class F> T Combine(T value,F)const{return value;}
    int First(bool value)const{return value?0:-1;}
    void AtomicMin(int& target,int value)const{if(value<target)target=value;}
    void AtomicMax(int& target,int value)const{if(value>target)target=value;}
    bool Claim(int& state)const{if(state!=1)return false;state=2;return true;}
    int Append(int& size)const{return size++;}
};
// An extreme keeps its first occurrence in the serial order. -0 and +0
// compare equal, so the position, not the value, must break that tie.
struct Extreme { double value; int order,padding; };
OKUFLOW_SURFACE_HD inline Extreme Lower(Extreme a,Extreme b){return b.value<a.value||(b.value==a.value&&b.order<a.order)?b:a;}
OKUFLOW_SURFACE_HD inline Extreme Upper(Extreme a,Extreme b){return b.value>a.value||(b.value==a.value&&b.order<a.order)?b:a;}
struct Hypothesis { double a,b; int count,order; };
struct Sums { double sx,sy,xx,xy; int count,first,last,padding; };
// Six deterministic anchors, at most 15 hypotheses and 96 observations.
// Rounded corners are outliers; a long, independently observed edge is needed.
template<class Team> OKUFLOW_SURFACE_HD Line FitLine(const Team& team,const int* values,int lo,int hi,double slopeLimit)
{
    const int trim=(hi-lo)/5;lo+=trim;hi-=trim;
    const int n=hi-lo+1;if(n<4)return {};
    // Anchor pairs (i,j), i<j<6, in their serial order; the first best wins.
    Hypothesis local{0,0,0,15};
    for(int order=team.Rank();order<15;order+=team.Size()){
        int i=0,j=0;for(int o=order;;++i){if(o<5-i){j=i+1+o;break;}o-=5-i;}
        const int p=lo+(n-1)*i/5,q=lo+(n-1)*j/5;
        if(q-p<Max(1,(n-1)/5)||values[p]<0||values[q]<0)continue;
        const double a=double(values[q]-values[p])/(q-p),b=values[p]-a*(p+.5);
        if(Abs(a)>slopeLimit)continue;
        int count=0;for(int k=lo;k<=hi;++k)if(values[k]>=0&&Abs(values[k]-a*(k+.5)-b)<=1)++count;
        if(count>local.count)local={a,b,count,order};
    }
    const auto chosen=team.Combine(local,[](Hypothesis x,Hypothesis y){
        return y.count>x.count||(y.count==x.count&&y.order<x.order)?y:x;});
    double bestA=chosen.a,bestB=chosen.b;const int best=chosen.count;
    if(best<4||best*5<n*3)return {};
    // Refit twice, dropping rounded-corner outliers after the first fit.
    // Both passes retain the same support/span and residual requirements.
    for(int pass=0;pass<2;++pass){
        Sums sums{0,0,0,0,0,hi,lo,0};
        for(int k=lo+team.Rank();k<=hi;k+=team.Size())if(values[k]>=0&&Abs(values[k]-bestA*(k+.5)-bestB)<=1){
            const double x=k+.5,y=values[k];sums.sx+=x;sums.sy+=y;sums.xx+=x*x;sums.xy+=x*y;++sums.count;
            sums.first=Min(sums.first,k);sums.last=Max(sums.last,k);
        }
        sums=team.Combine(sums,[](Sums x,Sums y){return Sums{x.sx+y.sx,x.sy+y.sy,x.xx+y.xx,x.xy+y.xy,
            x.count+y.count,Min(x.first,y.first),Max(x.last,y.last),0};});
        const double sx=sums.sx,sy=sums.sy,xx=sums.xx,xy=sums.xy;const int count=sums.count;
        const double den=count*xx-sx*sx;
        if(count<4||count*5<n*3||den<=0||sums.last-sums.first<(hi-lo)/2)return {};
        const double a=(count*xy-sx*sy)/den,b=(sy-a*sx)/count;
        if(!Finite(a)||!Finite(b)||Abs(a)>slopeLimit)return {};
        if(pass){int outlier=0;
            for(int k=lo+team.Rank();k<=hi;k+=team.Size())if(values[k]>=0&&Abs(values[k]-bestA*(k+.5)-bestB)<=1&&
                Abs(values[k]-a*(k+.5)-b)>1.25)outlier=1;
            if(team.Combine(outlier,[](int x,int y){return x|y;}))return {};
        }
        bestA=a;bestB=b;
    }
    return {bestA,bestB,true};
}
struct Homography {
    double m[9];
    OKUFLOW_SURFACE_HD bool Map(Point p,Point& out)const{
        const double z=m[6]*p.x+m[7]*p.y+m[8];if(!Finite(z)||Abs(z)<1e-8)return false;
        out={(m[0]*p.x+m[1]*p.y+m[2])/z,(m[3]*p.x+m[4]*p.y+m[5])/z};
        return Finite(out.x)&&Finite(out.y);
    }
    OKUFLOW_SURFACE_HD bool Inverse(Homography& o)const{
        const double* a=m;
        const double b[9]={a[4]*a[8]-a[5]*a[7],a[2]*a[7]-a[1]*a[8],a[1]*a[5]-a[2]*a[4],
            a[5]*a[6]-a[3]*a[8],a[0]*a[8]-a[2]*a[6],a[2]*a[3]-a[0]*a[5],
            a[3]*a[7]-a[4]*a[6],a[1]*a[6]-a[0]*a[7],a[0]*a[4]-a[1]*a[3]};
        const double d=a[0]*b[0]+a[1]*b[3]+a[2]*b[6];if(!Finite(d)||Abs(d)<1e-8)return false;
        for(int i=0;i<9;++i)o.m[i]=b[i]/d;return true;
    }
};
OKUFLOW_SURFACE_HD inline bool Frame(const Point* p,Homography& h)
{
    for(int i=0;i<4;++i){
        const auto a=p[i],b=p[(i+1)%4],c=p[(i+2)%4];
        if(!Finite(a.x)||!Finite(a.y)||(b.x-a.x)*(c.y-b.y)-(b.y-a.y)*(c.x-b.x)<=1e-6)return false;
    }
    const double x1=p[1].x-p[2].x,x2=p[3].x-p[2].x,x3=p[0].x-p[1].x+p[2].x-p[3].x;
    const double y1=p[1].y-p[2].y,y2=p[3].y-p[2].y,y3=p[0].y-p[1].y+p[2].y-p[3].y;
    const double d=x1*y2-x2*y1;if(Abs(d)<1e-8)return false;
    const double g=(x3*y2-x2*y3)/d,k=(x1*y3-x3*y1)/d;
    if(1+g<=.25||1+k<=.25||1+g+k<=.25)return false;
    h={{p[1].x-p[0].x+g*p[1].x,p[3].x-p[0].x+k*p[3].x,p[0].x,
        p[1].y-p[0].y+g*p[1].y,p[3].y-p[0].y+k*p[3].y,p[0].y,g,k,1}};return true;
}
// Floods one 4-connected paper component. Members expand the current level
// together; a level's cells are appended in any order, which changes neither
// the component nor its size, bounds or openness.
struct Component { int size,l,r,t,b,open; };
template<class Team> OKUFLOW_SURFACE_HD Component Flood(const Team& team,Scratch& s,unsigned short* queue,int seed)
{
    if(!team.Rank()){queue[0]=static_cast<unsigned short>(seed);s.state[seed]=2;s.size=1;}
    team.Sync();
    int begin=0,end=1,l=Columns,r=0,t=Rows,b=0,open=0;
    while(begin<end){
        for(int i=begin+team.Rank();i<end;i+=team.Size()){const int cell=queue[i],x=cell%Columns,y=cell/Columns;
            l=Min(l,x);r=Max(r,x);t=Min(t,y);b=Max(b,y);
            if(x==0||y==0||x==Columns-1||y==Rows-1)open=1;
            const int adjacent[4]={x?cell-1:-1,x+1<Columns?cell+1:-1,y?cell-Columns:-1,y+1<Rows?cell+Columns:-1};
            for(int v:adjacent)if(v>=0){if(s.state[v]==3)open=1;
                if(team.Claim(s.state[v]))queue[team.Append(s.size)]=static_cast<unsigned short>(v);}
        }
        team.Sync();begin=end;end=s.size;team.Sync();
    }
    return team.Combine(Component{end,l,r,t,b,open},[](Component x,Component y){
        return Component{x.size,Min(x.l,y.l),Max(x.r,y.r),Min(x.t,y.t),Max(x.b,y.b),x.open|y.open};});
}
// Sample receives local coordinates of the existing fitted word and returns
// actual source RGB. Missing pixels are unknown boundaries, never plane edges.
template<class Team,class Sample> OKUFLOW_SURFACE_HD Result RefineContext(const Team& team,double w,double h,Rgb fg,Rgb bg,
    Sample sample,Scratch& s,int verticalSpan)
{
    Result result{};result.stage=1;
    if(!Finite(w)||!Finite(h)||w<8||h<8||w>1024||h>256||w/h>24)return result;
    result.stage=2;if(Max(Max(int(Abs(fg.r-bg.r)),int(Abs(fg.g-bg.g))),int(Abs(fg.b-bg.b)))<64)return result;
    const double x0=-2*verticalSpan*h,y0=-(verticalSpan-1)*.5*h;
    const double dx=(w+4*verticalSpan*h)/Columns,dy=verticalSpan*h/Rows;
    const auto compatible=[&](Rgb c){return Abs(c.r-bg.r)<=24&&Abs(c.g-bg.g)<=24&&Abs(c.b-bg.b)<=24;};
    for(int cell=team.Rank();cell<Cells;cell+=team.Size()){const int x=cell%Columns,y=cell/Columns;Rgb c;
        const bool valid=sample(x0+(x+.5)*dx,y0+(y+.5)*dy,c);
        s.state[cell]=valid?(compatible(c)?1:0):3;
    }
    team.Sync();
    const double cx=(w*.5-x0)/dx,cy=(h*.5-y0)/dy;
    int best=0,bx0=0,bx1=0,by0=0,by1=0,active=0,kept=0;
    // Seeds are taken in raster order, exactly as a serial scan finds them;
    // flooding only turns paper cells 1 into visited cells 2.
    for(int base=0;base<Cells;base+=team.Size())for(;;){
        const int cell=base+team.Rank(),first=team.First(cell<Cells&&s.state[cell]==1);
        if(first<0)break;
        const auto component=Flood(team,s,s.queue[active],base+first);
        const int size=component.size,l=component.l,r=component.r,t=component.t,b=component.b;
        const bool open=component.open!=0;
        if(open||size<=best||size<64||cx<l||cx>r+1||cy<t||cy>b+1||size*dx*dy<w*h*1.2)continue;
        best=size;bx0=l;bx1=r;by0=t;by1=b;kept=active;active^=1;
    }
    result.stage=3;if(!best)return result;
    for(int i=team.Rank();i<Rows;i+=team.Size()){s.left[i]=Columns;s.right[i]=-1;}
    for(int i=team.Rank();i<Columns;i+=team.Size()){s.top[i]=Rows;s.bottom[i]=-1;}
    team.Sync();
    for(int i=team.Rank();i<best;i+=team.Size()){const int x=s.queue[kept][i]%Columns,y=s.queue[kept][i]/Columns;
        team.AtomicMin(s.left[y],x);team.AtomicMax(s.right[y],x+1);
        team.AtomicMin(s.top[x],y);team.AtomicMax(s.bottom[x],y+1);
    }
    team.Sync();
    for(int i=team.Rank();i<Rows;i+=team.Size())if(s.left[i]==Columns)s.left[i]=-1;
    for(int i=team.Rank();i<Columns;i+=team.Size())if(s.top[i]==Rows)s.top[i]=-1;
    team.Sync();
    const auto left=FitLine(team,s.left,by0,by1,2.5*dy/dx),right=FitLine(team,s.right,by0,by1,2.5*dy/dx);
    const auto top=FitLine(team,s.top,bx0,bx1,.5*dx/dy),bottom=FitLine(team,s.bottom,bx0,bx1,.5*dx/dy);
    result.stage=41;if(!left.valid)return result;
    result.stage=42;if(!right.valid)return result;
    result.stage=43;if(!top.valid)return result;
    result.stage=44;if(!bottom.valid)return result;
    const Line sides[4]={left,right,right,left},edges[4]={top,top,bottom,bottom};Point corners[4];
    for(int i=0;i<4;++i){const auto a=sides[i],b=edges[i];const double den=1-a.a*b.a;
        if(Abs(den)<.25)return result;const double x=(a.a*b.b+a.b)/den,y=b.a*x+b.b;
        corners[i]={x0+x*dx,y0+y*dy};
    }
    result.stage=5;Homography plane,inverse;if(!Frame(corners,plane)||!plane.Inverse(inverse))return result;
    const int threshold=(77*(fg.r+bg.r)+150*(fg.g+bg.g)+29*(fg.b+bg.b))/512;
    const bool dark=77*fg.r+150*fg.g+29*fg.b<77*bg.r+150*bg.g+29*bg.b;
    int step=1;while((int(w/step)+1)*(int(h/step)+1)>16384)++step;
    // The serial sample loops visit x,y = 0, step, ... < w,h; integer
    // multiples are exact, so row-major order is index = row*nx + column.
    int nx=0,ny=0;while(double(nx*step)<w)++nx;while(double(ny*step)<h)++ny;
    Extreme u0{1e100,0x7fffffff,0},v0=u0,u1{-1e100,0x7fffffff,0},v1=u1;
    int count=0,failed=0,previous=-2;Point reuse[2];
    int column=team.Rank()%nx,row=team.Rank()/nx;
    for(int index=team.Rank();index<nx*ny;index+=team.Size()){
        const double x=double(column*step),y=double(row*step);const int at=column;
        column+=team.Size();while(column>=nx){column-=nx;++row;}
        Rgb c;if(!sample(x,y,c)){failed=1;break;}
        const int luma=(77*c.r+150*c.g+29*c.b)/256;if(dark?luma>=threshold:luma<=threshold)continue;
        ++count;
        // A left neighbour already mapped this cell's two left corners from
        // bit-identical points; reusing them changes no mapped value.
        const bool shared=previous==index-1&&at>0;previous=index;
        const Point neighbour[2]={reuse[0],reuse[1]};
        for(int k=0;k<4;++k){Point uv;
            if(shared&&(k==0||k==3))uv=neighbour[k==3];
            else{const Point p{x+((k==1||k==2)?(x+step>w?w-x:step):0),
                y+(k>=2?(y+step>h?h-y:step):0)};if(!inverse.Map(p,uv)){failed=1;break;}}
            if(k==1)reuse[0]=uv;if(k==2)reuse[1]=uv;
            const int order=index*4+k;
            u0=Lower(u0,{uv.x,order,0});u1=Upper(u1,{uv.x,order,0});
            v0=Lower(v0,{uv.y,order,0});v1=Upper(v1,{uv.y,order,0});
        }
        if(failed)break;
    }
    struct Ink { Extreme u0,v0,u1,v1; int count,failed; };
    const auto ink=team.Combine(Ink{u0,v0,u1,v1,count,failed},[](Ink a,Ink b){
        return Ink{Lower(a.u0,b.u0),Lower(a.v0,b.v0),Upper(a.u1,b.u1),Upper(a.v1,b.v1),a.count+b.count,a.failed|b.failed};});
    if(ink.failed)return result;
    count=ink.count;
    const double U0=ink.u0.value,V0=ink.v0.value,U1=ink.u1.value,V1=ink.v1.value;
    result.stage=6;if(count<16||U0<0||V0<0||U1>1||V1>1||U1<=U0||V1<=V0)return result;
    const Point box[4]={{U0,V0},{U1,V0},{U1,V1},{U0,V1}};
    result.stage=7;for(int i=0;i<4;++i)if(!plane.Map(box[i],result.quad[i]))return result;
    // A rectified ink envelope legitimately projects outside its old rotated
    // rectangle. Bound that excursion by the independently fitted side edges,
    // rather than applying a half-height limit that assumes upright letters.
    const double leftHeight=corners[3].y-corners[0].y,rightHeight=corners[2].y-corners[1].y;
    result.stage=8;if(leftHeight<=1e-6||rightHeight<=1e-6)return result;
    const double leftShear=Abs((corners[3].x-corners[0].x)/leftHeight);
    const double rightShear=Abs((corners[2].x-corners[1].x)/rightHeight);
    const double shear=leftShear>rightShear?leftShear:rightShear;
    const double horizontalMargin=.5*h*(1+shear);
    double minx=1e100,miny=1e100,maxx=-1e100,maxy=-1e100,shape=0;
    for(int i=0;i<4;++i){const auto p=result.quad[i];
        if(p.x<-horizontalMargin||p.x>w+horizontalMargin||p.y<-.5*h||p.y>h*1.5)return result;
        if(p.x<minx)minx=p.x;if(p.x>maxx)maxx=p.x;if(p.y<miny)miny=p.y;if(p.y>maxy)maxy=p.y;
    }
    result.stage=9;const double area=(maxx-minx)*(maxy-miny);if(area<w*h*.5||area>w*h*1.75)return result;
    shape=Abs(result.quad[3].x-result.quad[0].x)+Abs(result.quad[2].x-result.quad[1].x)+
        Abs(result.quad[2].y-result.quad[1].y-result.quad[3].y+result.quad[0].y);
    result.stage=10;if(shape<2||shape<h*.1)return result;
    result.fitted=true;return result;
}
template<class Team,class Sample> OKUFLOW_SURFACE_HD Result Refine(const Team& team,double w,double h,Rgb fg,Rgb bg,
    Sample sample,Scratch& s)
{
    const auto first=RefineContext(team,w,h,fg,bg,sample,s,4);
    // Small print on a larger card can put every boundary outside the first
    // context. One larger context is allowed only when no enclosing component
    // was found, never to override a rejected plane or a nearly upright result.
    return first.stage==3?RefineContext(team,w,h,fg,bg,sample,s,8):first;
}
template<class Sample> Result Refine(double w,double h,Rgb fg,Rgb bg,Sample sample,Scratch& s)
{
    return Refine(SerialTeam{},w,h,fg,bg,sample,s);
}
} // namespace okuflow::uct::surface_geometry
#undef OKUFLOW_SURFACE_HD
