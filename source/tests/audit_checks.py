"""Host checks of extracted production math/parser and static lifecycle guards.
Not a Windows/D3D runtime test. Requires Python 3 and a native g++.
"""
from pathlib import Path
import subprocess, tempfile, re
src=Path(__file__).resolve().parents[1]/'src/main.cpp'
s=src.read_text()
struct=s[s.index('struct alignas(16) ShaderConstants'):s.index('\n};',s.index('struct alignas(16) ShaderConstants'))+3]
parser=s[s.index('bool ParseFiniteFloat('):s.index('\n}',s.index('bool ParseFiniteFloat('))+2]
a=s.index('        auto makeWeights = []');b=s.index('\n        };',a)+len('\n        };')
weights=s[a:b]
harness=r'''
#include <algorithm>
#include <cmath>
#include <cerrno>
#include <cwctype>
#include <cstdlib>
#include <cassert>
#include <iostream>
STRUCT
PARSER
int main() {
    float v=0;
    for (const wchar_t* t : {L"nan",L"inf",L"-inf",L"1e1000",L"24abc",L"",L" ",L"6,5"})
        assert(!ParseFiniteFloat(t,&v));
    for (const wchar_t* t : {L"6.5",L" 6.5 ",L"0",L"100",L"-0"})
        assert(ParseFiniteFloat(t,&v) && std::isfinite(v));
    WEIGHTS
    int cases=0;
    for (float sigma : {0.05f,0.2f,0.8f,1.3f,2.12f,6.0f,16.0f,31.9f,32.0f}) {
        const int radius=std::clamp(int(std::ceil(3*sigma)),1,96);
        for (int pairCount : {(radius+1)/2,48}) {
            ShaderConstants c{};
            makeWeights(sigma,radius,pairCount,0,c);
            makeWeights(sigma,radius,pairCount,1,c);
            double expected[97]{}, total=0;
            for(int i=0;i<=radius;++i) {expected[i]=std::exp(-0.5*double(i*i)/(sigma*sigma));total+=expected[i]*(i?2:1);}
            for(int i=0;i<=radius;++i) expected[i]/=total;
            for(int ch=0;ch<2;++ch) {
                double actual[97]{}; actual[0]=c.weights[0][ch];
                double sum=actual[0];
                for(int p=1;p<=pairCount;++p) {
                    const double w=c.weights[p][ch], offset=c.weights[p][ch+2];
                    assert(std::isfinite(w) && std::isfinite(offset) && w>=0);
                    if(w==0) continue;
                    // Reconstruct exactly the two adjacent taps packed by the real code.
                    const int a=2*p-1,b=2*p;
                    assert(offset>=a-1e-5 && offset<=b+1e-5);
                    const double fraction=std::clamp(offset-a,0.0,1.0);
                    if(a<=96) actual[a]+=w*(1-fraction);
                    if(b<=96) actual[b]+=w*fraction;
                    sum+=2*w;
                }
                assert(std::abs(sum-1)<2e-6);
                for(int i=0;i<=96;++i) assert(std::abs(actual[i]-expected[i])<2e-6);
            }
            ++cases;
        }
    }
    std::cout<<"PASS: malformed/finite parameter parsing; "<<cases<<" Gaussian kernel/pair cases\n";
}
'''.replace('STRUCT',struct).replace('PARSER',parser).replace('WEIGHTS',weights)
with tempfile.TemporaryDirectory() as d:
    cpp=Path(d)/'math_checks.cpp';exe=Path(d)/'math_checks';cpp.write_text(harness)
    subprocess.run(['g++','-std=c++20','-O1','-g','-fsanitize=address,undefined',str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
# Core regressions identifiable directly in the production source.
draw=s[s.index('    bool Draw('):s.index('    void SetScissor',s.index('    bool Draw('))]
assert 'backRtv_' not in s and 'composedRtv_.GetAddressOf()' in draw
assert 'if (FAILED(hr)) return true;' not in s[s.index('    bool RenderFrame'):s.index('    bool CollectDirtyRects')]
assert 'CloseHandle(frameLatencyWaitable_)' in s
assert 'gRenderer.InvalidatePresentation();' in s
assert 'SingleInstance' in s and 'FindWindowW(kClassName' in s
assert 'candidate.disjoint && !candidate.pending' in s
assert 'Myopic Defocus GPU' not in s
assert 'std::filesystem::copy_file(legacy, target' in s
# Simple state regression: full updates must also refresh the persistent image.
old=lambda cache, full: cache
new=lambda cache, full: full.copy()
for update in (['first']*8,['new-strength']*8):
    cache=new(['unset']*8,update);cache[3]='dirty'
    assert cache[:3]==update[:3] and cache[4:]==update[4:]
print('PASS: composition path, recovery guards, handle cleanup, fresh enable, singleton, timing slots, naming/migration static checks')
