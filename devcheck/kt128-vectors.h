/* Public numerical vectors, RFC 9861 section 5. ptn(i)=i mod 251. */
#ifndef CHECK_KT128_VECTORS_H
#define CHECK_KT128_VECTORS_H
struct k12_vector { size_t message_len, custom_len; const char *hex32; };
static const struct k12_vector k12_vectors[] = {
 {0,0,"1ac2d450fc3b4205d19da7bfca1b37513c0803577ac7167f06fe2ce1f0ef39e5"},
 {17,0,"6bf75fa2239198db4772e36478f8e19b0f371205f6a9a93a273f51df37122888"},
 {8191,0,"1b577636f723643e990cc7d6a659837436fd6a103626600eb8301cd1dbe553d6"},
 {8192,0,"48f256f6772f9edfb6a8b661ec92dc93b95ebd05a08a17b39ae3490870c926c3"},
 {8192,8189,"3ed12f70fb05ddb58689510ab3e4d23c6c6033849aa01e1d8c220a297fedcd0b"},
 {8192,8190,"6a7c1b6a5cd0d8c9ca943a4a216cc64604559a2ea45f78570a15253d67ba00ae"}
};
static const char k12_empty_10032_tail32[] = "e8dc563642f7228c84684c898405d3a834799158c079b12880277a1d28e2ff6d";
#endif
