#include "okuflow/common/uct_formula_artifact.hpp"
#include <QGuiApplication>
#include <QPainterPathStroker>
#include <QRegularExpression>
#include <QSet>
#include <mutex>
#include <stdexcept>
#include "latex.h"
#include "platform/qt/graphic_qt.h"

// Resource initialization must be in the global namespace.
static void InitializeMathResources() { Q_INIT_RESOURCE(okuflow_math_fonts); }

namespace okuflow::uct::formula {
namespace {
bool Safe(const QString& text) {
    if(text.isEmpty() || text.size()>8192 || text.contains(QChar::Null))return false;
    int depth=0;
    for(QChar c:text) {
        if(c=='{' && ++depth>32)return false;
        if(c=='}' && --depth<0)return false;
        if(c.isLowSurrogate() || c.isHighSurrogate() || (c.unicode()<32 && !c.isSpace()))return false;
    }
    if(depth)return false;
    static const QSet<QString> allowed={
        "frac","dfrac","tfrac","sqrt","left","right","middle","cdot","cdots","ldots","vdots","ddots","times","div","pm","mp",
        "sum","prod","int","iint","iiint","oint","limits","nolimits","infty","partial","nabla","approx","equiv","neq","ne","le","ge","leq","geq",
        "sin","cos","tan","cot","sec","csc","arcsin","arccos","arctan","sinh","cosh","tanh","log","ln","exp","lim","max","min","sup","inf","det","gcd",
        "alpha","beta","gamma","delta","epsilon","varepsilon","zeta","eta","theta","vartheta","iota","kappa","lambda","mu","nu","xi","pi","varpi","rho","varrho","sigma","varsigma","tau","upsilon","phi","varphi","chi","psi","omega",
        "Gamma","Delta","Theta","Lambda","Xi","Pi","Sigma","Upsilon","Phi","Psi","Omega",
        "hat","widehat","bar","overline","underline","vec","dot","ddot","tilde","widetilde","breve","acute","grave","check",
        "mathrm","mathbf","mathit","mathsf","mathtt","mathcal","mathbb","mathfrak","boldsymbol","operatorname","text","textrm","textbf","textit",
        "quad","qquad","displaystyle","textstyle","scriptstyle","scriptscriptstyle","substack","overset","underset","overbrace","underbrace",
        "binom","dbinom","tbinom","not","in","notin","ni","subset","supset","subseteq","supseteq","cup","cap","emptyset","varnothing","forall","exists",
        "to","rightarrow","leftarrow","leftrightarrow","Rightarrow","Leftarrow","Leftrightarrow","mapsto","longrightarrow","longleftarrow","Longrightarrow",
        "langle","rangle","lvert","rvert","lVert","rVert","vert","Vert","ell","hbar","imath","jmath","Re","Im","perp","parallel","propto","circ","degree",
        "begin","end"
    };
    static const QRegularExpression commands(QStringLiteral("\\\\([A-Za-z]+|.)"));
    auto matches=commands.globalMatch(text);
    while(matches.hasNext()) {
        const auto command=matches.next().captured(1);
        if(command.size()>1 && !allowed.contains(command))return false;
        if(command.size()==1 && command[0].isLetter() && !allowed.contains(command))return false;
    }
    // Bound array-like structures; no user macros, imports, fonts or external files.
    static const QRegularExpression environment(QStringLiteral("\\\\(?:begin|end)\\s*\\{([^}]+)\\}"));
    auto envs=environment.globalMatch(text);
    static const QSet<QString> names={"matrix","pmatrix","bmatrix","Bmatrix","vmatrix","Vmatrix","smallmatrix","cases","aligned","gathered","array"};
    while(envs.hasNext())if(!names.contains(envs.next().captured(1)))return false;
    return text.count('&')<=128 && text.count(QStringLiteral("\\\\"))<=128;
}
class PathGraphics final:public tex::Graphics2D {
public:
    QPainterPath path;
    tex::Font_qt defaultFont{"Serif",tex::PLAIN,20};
    const tex::Font* font{&defaultFont};
    tex::Stroke stroke;
    tex::color color{0xff000000};
    QTransform transform;
    float scaleX{1},scaleY{1};
    void add(const QPainterPath& p) {
        if(p.elementCount()>200000 || path.elementCount()>200000-p.elementCount())
            throw std::runtime_error("Formula path exceeds its bound.");
        path.addPath(transform.map(p));
    }
    void setColor(tex::color c) override {color=c;}
    tex::color getColor() const override {return color;}
    void setStroke(const tex::Stroke& s) override {stroke=s;}
    const tex::Stroke& getStroke() const override {return stroke;}
    void setStrokeWidth(float w) override {stroke.lineWidth=w;}
    void setFont(const tex::Font* f) override {font=f;}
    const tex::Font* getFont() const override {return font;}
    void translate(float x,float y) override {transform.translate(x,y);}
    void scale(float x,float y) override {transform.scale(x,y);scaleX*=x;scaleY*=y;}
    void rotate(float a) override {transform.rotateRadians(a);}
    void rotate(float a,float x,float y) override {translate(x,y);rotate(a);translate(-x,-y);}
    void reset() override {transform.reset();scaleX=scaleY=1;}
    float sx() const override {return scaleX;}
    float sy() const override {return scaleY;}
    void drawChar(wchar_t c,float x,float y) override {drawText(std::wstring(1,c),x,y);}
    void drawText(const std::wstring& text,float x,float y) override {
        const auto* qtFont=dynamic_cast<const tex::Font_qt*>(font);
        if(!qtFont)throw std::runtime_error("Unsupported formula font.");
        QPainterPath p;p.addText(QPointF(x,y),qtFont->getQFont(),QString::fromStdWString(text));add(p);
    }
    void outline(const QPainterPath& p) {
        QPainterPathStroker pen;pen.setWidth(stroke.lineWidth>0?stroke.lineWidth:1);
        pen.setCapStyle(Qt::FlatCap);pen.setJoinStyle(Qt::MiterJoin);add(pen.createStroke(p));
    }
    void drawLine(float x,float y,float x2,float y2) override {QPainterPath p;p.moveTo(x,y);p.lineTo(x2,y2);outline(p);}
    void drawRect(float x,float y,float w,float h) override {QPainterPath p;p.addRect(x,y,w,h);outline(p);}
    void fillRect(float x,float y,float w,float h) override {QPainterPath p;p.addRect(x,y,w,h);add(p);}
    void drawRoundRect(float x,float y,float w,float h,float rx,float ry) override {QPainterPath p;p.addRoundedRect(QRectF(x,y,w,h),rx,ry);outline(p);}
    void fillRoundRect(float x,float y,float w,float h,float rx,float ry) override {QPainterPath p;p.addRoundedRect(QRectF(x,y,w,h),rx,ry);add(p);}
};
}
Layout Typeset(const QString& latex) {
    Layout result;
    try {
        if(!Safe(latex))throw std::runtime_error("Unsupported or oversized formula layout.");
        static std::mutex mutex;std::lock_guard lock(mutex);
        static const bool initialized=[] {
            InitializeMathResources();tex::LaTeX::init(":/okuflow-math");tex::LaTeX::setDebug(false);return true;
        }();
        Q_UNUSED(initialized);
        std::unique_ptr<tex::TeXRender> render(tex::LaTeX::parse(latex.toStdWString(),16384,64,0,0xff000000));
        if(!render || render->getWidth()>16384 || render->getHeight()>16384)throw std::runtime_error("Formula layout exceeds its bound.");
        PathGraphics graphics;render->draw(graphics,0,0);
        if(graphics.path.isEmpty())throw std::runtime_error("Formula has no supported vector layout.");
        const auto bounds=graphics.path.boundingRect();
        if(!bounds.isValid() || bounds.width()>16384 || bounds.height()>16384)throw std::runtime_error("Formula vector bounds are invalid.");
        QTransform normalize;normalize.scale(1/bounds.width(),1/bounds.height());normalize.translate(-bounds.x(),-bounds.y());
        result.path=std::make_shared<QPainterPath>(normalize.map(graphics.path));
        result.size=bounds.size();
    } catch(const std::exception& error) {result.error=QString::fromUtf8(error.what());}
    return result;
}
} // namespace okuflow::uct::formula
