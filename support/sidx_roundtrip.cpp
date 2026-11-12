#include "Ap4.h"
#include "Ap4SidxAtom.h"
#include <cstdio>
int main(){
 AP4_SidxAtom input(7,48000,0x100000001ULL,0x100000020ULL);
 input.SetReferenceCount(2);
 AP4_SidxAtom::Reference a;a.m_ReferencedSize=123;a.m_SubsegmentDuration=480;a.m_StartsWithSap=true;a.m_SapType=1;
 input.SetReference(0,a);a.m_ReferenceType=1;a.m_ReferencedSize=44;a.m_SubsegmentDuration=960;input.SetReference(1,a);
 auto stream=new AP4_MemoryByteStream();
 if(AP4_FAILED(input.Write(*stream)) || AP4_FAILED(stream->Seek(0)))return 1;
 AP4_Atom *atom=nullptr;AP4_DefaultAtomFactory factory;auto result=factory.CreateAtomFromStream(*stream,atom);
 if(AP4_FAILED(result)||!atom)return 2;
 auto out=AP4_DYNAMIC_CAST(AP4_SidxAtom,atom);
 if(!out || out->GetVersion()!=1 || out->GetTimeScale()!=48000 || out->GetEarliestPresentationTime()!=0x100000001ULL || out->GetFirstOffset()!=0x100000020ULL)return 3;
 if(out->GetReferences().ItemCount()!=2 || out->GetReferences()[0].m_ReferencedSize!=123 || out->GetReferences()[1].m_ReferenceType!=1)return 4;
 std::puts("BASELINE_BENTO4_SIDX_V1_64BIT_REFS_ROUNDTRIP_OK");
 delete atom;stream->Release();return 0;
}
