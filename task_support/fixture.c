/* Authored minimal glTF input; no model, animation evaluator or BVH is provided. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static void bytes(FILE *f, const void *p, size_t n) { if (fwrite(p,1,n,f)!=n) exit(2); }
int main(void) {
    float positions[12]={0,0,0,99, 1,0,0,99, 0,1,0,99};
    uint8_t joints[12]={0,1,0,0, 0,1,0,0, 0,1,0,0};
    uint8_t weights[12]={128,127,0,0, 128,127,0,0, 128,127,0,0};
    uint16_t indices[4]={0,1,2,0};
    float ibm[32]={0}; for(int j=0;j<2;j++) for(int i=0;i<4;i++) ibm[j*16+i*5]=1;
    ibm[12]=-2; ibm[16+13]=-3;
    uint8_t sparse[4]={1,0,0,0}; float delta[3]={0,0,2};
    FILE *f=fopen("buffer.bin","wb"); if(!f)return 2;
    bytes(f,positions,sizeof positions); bytes(f,joints,sizeof joints); bytes(f,weights,sizeof weights);
    bytes(f,indices,sizeof indices); bytes(f,ibm,sizeof ibm); bytes(f,sparse,sizeof sparse); bytes(f,delta,sizeof delta);
    if(fclose(f))return 2;
    const char *json=
      "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"uri\":\"buffer.bin\",\"byteLength\":224}],"
      "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":48,\"byteStride\":16},"
      "{\"buffer\":0,\"byteOffset\":48,\"byteLength\":12},{\"buffer\":0,\"byteOffset\":60,\"byteLength\":12},"
      "{\"buffer\":0,\"byteOffset\":72,\"byteLength\":6},{\"buffer\":0,\"byteOffset\":80,\"byteLength\":128},"
      "{\"buffer\":0,\"byteOffset\":208,\"byteLength\":1},{\"buffer\":0,\"byteOffset\":212,\"byteLength\":12}],"
      "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
      "{\"bufferView\":1,\"componentType\":5121,\"count\":3,\"type\":\"VEC4\"},"
      "{\"bufferView\":2,\"componentType\":5121,\"normalized\":true,\"count\":3,\"type\":\"VEC4\"},"
      "{\"bufferView\":3,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"},"
      "{\"bufferView\":4,\"componentType\":5126,\"count\":2,\"type\":\"MAT4\"},"
      "{\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"sparse\":{\"count\":1,"
      "\"indices\":{\"bufferView\":5,\"componentType\":5121},\"values\":{\"bufferView\":6}}}],"
      "\"meshes\":[{\"weights\":[0.25],\"primitives\":[{\"attributes\":{\"POSITION\":0,\"JOINTS_0\":1,\"WEIGHTS_0\":2},"
      "\"indices\":3,\"targets\":[{\"POSITION\":5}]}]}],"
      "\"skins\":[{\"joints\":[2,3],\"inverseBindMatrices\":4}],"
      "\"nodes\":[{\"mesh\":0,\"skin\":0,\"translation\":[100,0,0],\"weights\":[0]},"
      "{\"mesh\":0,\"skin\":0,\"translation\":[200,0,0],\"weights\":[0.5]},"
      "{\"translation\":[2,0,0]},{\"translation\":[0,3,0]},"
      "{\"mesh\":0,\"translation\":[-10,0,0],\"weights\":[0]}],"
      "\"scenes\":[{\"nodes\":[0,1,2,3,4]}],\"scene\":0}";
    f=fopen("scene.gltf","wb"); if(!f)return 2; bytes(f,json,strlen(json)); return fclose(f)?2:0;
}
