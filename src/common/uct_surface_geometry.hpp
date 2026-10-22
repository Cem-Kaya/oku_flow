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
    unsigned char state[Cells];
    unsigned short queue[Cells];
    int left[Rows],right[Rows],top[Columns],bottom[Columns];
};
struct Result { Point quad[4]; bool fitted{}; int stage{}; };
struct Line { double a{},b{}; bool valid{}; };
OKUFLOW_SURFACE_HD inline double Abs(double x){return x<0?-x:x;}
OKUFLOW_SURFACE_HD inline int Min(int a,int b){return a<b?a:b;}
OKUFLOW_SURFACE_HD inline int Max(int a,int b){return a>b?a:b;}
OKUFLOW_SURFACE_HD inline bool Finite(double x){return x==x && Abs(x)<1e100;}
// Six deterministic anchors, at most 15 hypotheses and 96 observations.
// Rounded corners are outliers; a long, independently observed edge is needed.
OKUFLOW_SURFACE_HD inline Line FitLine(const int* values,int lo,int hi,double slopeLimit)
{
    const int trim=(hi-lo)/5;lo+=trim;hi-=trim;
    const int n=hi-lo+1;if(n<4)return {};
    double bestA=0,bestB=0;int best=0;
    for(int i=0;i<6;++i)for(int j=i+1;j<6;++j){
        const int p=lo+(n-1)*i/5,q=lo+(n-1)*j/5;
        if(q-p<Max(1,(n-1)/5)||values[p]<0||values[q]<0)continue;
        const double a=double(values[q]-values[p])/(q-p),b=values[p]-a*(p+.5);
        if(Abs(a)>slopeLimit)continue;
        int count=0;for(int k=lo;k<=hi;++k)if(values[k]>=0&&Abs(values[k]-a*(k+.5)-b)<=1)++count;
        if(count>best){best=count;bestA=a;bestB=b;}
    }
    if(best<4||best*5<n*3)return {};
    // Refit twice, dropping rounded-corner outliers after the first fit.
    // Both passes retain the same support/span and residual requirements.
    for(int pass=0;pass<2;++pass){
        double sx=0,sy=0,xx=0,xy=0;int first=hi,last=lo,count=0;
        for(int k=lo;k<=hi;++k)if(values[k]>=0&&Abs(values[k]-bestA*(k+.5)-bestB)<=1){
            const double x=k+.5,y=values[k];sx+=x;sy+=y;xx+=x*x;xy+=x*y;++count;
            first=Min(first,k);last=Max(last,k);
        }
        const double den=count*xx-sx*sx;
        if(count<4||count*5<n*3||den<=0||last-first<(hi-lo)/2)return {};
        const double a=(count*xy-sx*sy)/den,b=(sy-a*sx)/count;
        if(!Finite(a)||!Finite(b)||Abs(a)>slopeLimit)return {};
        if(pass)for(int k=lo;k<=hi;++k)if(values[k]>=0&&Abs(values[k]-bestA*(k+.5)-bestB)<=1&&
            Abs(values[k]-a*(k+.5)-b)>1.25)return {};
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
// Sample receives local coordinates of the existing fitted word and returns
// actual source RGB. Missing pixels are unknown boundaries, never plane edges.
template<class Sample> OKUFLOW_SURFACE_HD Result RefineContext(double w,double h,Rgb fg,Rgb bg,
    Sample sample,Scratch& s,int verticalSpan)
{
    Result result{};result.stage=1;
    if(!Finite(w)||!Finite(h)||w<8||h<8||w>1024||h>256||w/h>24)return result;
    result.stage=2;if(Max(Max(int(Abs(fg.r-bg.r)),int(Abs(fg.g-bg.g))),int(Abs(fg.b-bg.b)))<64)return result;
    const double x0=-2*verticalSpan*h,y0=-(verticalSpan-1)*.5*h;
    const double dx=(w+4*verticalSpan*h)/Columns,dy=verticalSpan*h/Rows;
    const auto compatible=[&](Rgb c){return Abs(c.r-bg.r)<=24&&Abs(c.g-bg.g)<=24&&Abs(c.b-bg.b)<=24;};
    for(int y=0;y<Rows;++y)for(int x=0;x<Columns;++x){Rgb c;
        const bool valid=sample(x0+(x+.5)*dx,y0+(y+.5)*dy,c);
        s.state[y*Columns+x]=valid?(compatible(c)?1:0):3;
    }
    const double cx=(w*.5-x0)/dx,cy=(h*.5-y0)/dy;
    int best=0,bx0=0,bx1=0,by0=0,by1=0;
    for(int seed=0;seed<Cells;++seed){
        if(s.state[seed]!=1)continue;
        int read=0,size=1,l=Columns,r=0,t=Rows,b=0;bool open=false;
        s.queue[0]=static_cast<unsigned short>(seed);s.state[seed]=2;
        while(read<size){const int cell=s.queue[read++],x=cell%Columns,y=cell/Columns;
            l=Min(l,x);r=Max(r,x);t=Min(t,y);b=Max(b,y);
            if(x==0||y==0||x==Columns-1||y==Rows-1)open=true;
            const int adjacent[4]={x?cell-1:-1,x+1<Columns?cell+1:-1,y?cell-Columns:-1,y+1<Rows?cell+Columns:-1};
            for(int v:adjacent)if(v>=0){if(s.state[v]==3)open=true;if(s.state[v]==1){s.state[v]=2;s.queue[size++]=static_cast<unsigned short>(v);}}
        }
        if(open||size<=best||size<64||cx<l||cx>r+1||cy<t||cy>b+1||size*dx*dy<w*h*1.2)continue;
        best=size;bx0=l;bx1=r;by0=t;by1=b;
        for(int y=0;y<Rows;++y){s.left[y]=-1;s.right[y]=-1;}
        for(int x=0;x<Columns;++x){s.top[x]=-1;s.bottom[x]=-1;}
        for(int i=0;i<size;++i){const int x=s.queue[i]%Columns,y=s.queue[i]/Columns;
            if(s.left[y]<0||x<s.left[y])s.left[y]=x;s.right[y]=Max(s.right[y],x+1);
            if(s.top[x]<0||y<s.top[x])s.top[x]=y;s.bottom[x]=Max(s.bottom[x],y+1);
        }
    }
    result.stage=3;if(!best)return result;
    const auto left=FitLine(s.left,by0,by1,2.5*dy/dx),right=FitLine(s.right,by0,by1,2.5*dy/dx);
    const auto top=FitLine(s.top,bx0,bx1,.5*dx/dy),bottom=FitLine(s.bottom,bx0,bx1,.5*dx/dy);
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
    double u0=1e100,v0=1e100,u1=-1e100,v1=-1e100;int count=0;
    const int threshold=(77*(fg.r+bg.r)+150*(fg.g+bg.g)+29*(fg.b+bg.b))/512;
    const bool dark=77*fg.r+150*fg.g+29*fg.b<77*bg.r+150*bg.g+29*bg.b;
    int step=1;while((int(w/step)+1)*(int(h/step)+1)>16384)++step;
    for(double y=0;y<h;y+=step)for(double x=0;x<w;x+=step){Rgb c;if(!sample(x,y,c))return result;
        const int luma=(77*c.r+150*c.g+29*c.b)/256;if(dark?luma>=threshold:luma<=threshold)continue;
        ++count;
        for(int k=0;k<4;++k){Point uv;const Point p{x+((k==1||k==2)?(x+step>w?w-x:step):0),
            y+(k>=2?(y+step>h?h-y:step):0)};if(!inverse.Map(p,uv))return result;
            if(uv.x<u0)u0=uv.x;if(uv.x>u1)u1=uv.x;if(uv.y<v0)v0=uv.y;if(uv.y>v1)v1=uv.y;
        }
    }
    result.stage=6;if(count<16||u0<0||v0<0||u1>1||v1>1||u1<=u0||v1<=v0)return result;
    const Point box[4]={{u0,v0},{u1,v0},{u1,v1},{u0,v1}};
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
template<class Sample> OKUFLOW_SURFACE_HD Result Refine(double w,double h,Rgb fg,Rgb bg,Sample sample,Scratch& s)
{
    const auto first=RefineContext(w,h,fg,bg,sample,s,4);
    // Small print on a larger card can put every boundary outside the first
    // context. One larger context is allowed only when no enclosing component
    // was found, never to override a rejected plane or a nearly upright result.
    return first.stage==3?RefineContext(w,h,fg,bg,sample,s,8):first;
}
} // namespace okuflow::uct::surface_geometry
#undef OKUFLOW_SURFACE_HD
