
![Little CMS](doc/logo-small.png)

# About Little CMS
[www.littlecms.com](https://www.littlecms.com)

Little CMS intends to be an **OPEN SOURCE** small-footprint color management engine, with special focus on accuracy and performance. It uses the International Color Consortium standard (ICC), which is the modern standard when regarding to color management. The ICC specification is widely used and is referred to in many International and other de-facto standards. It was approved as an International Standard, ISO 15076-1, in 2005. 



# Conformance
Little CMS is a **FULL IMPLEMENTATION** of ICC specification 4.4, it fully supports all kind of V2 and V4 profiles, including abstract, devicelink and named color profiles. Check the tutorial for a exhaustive list of features. 


# A bit of story
Since the initial release, back in 1998, Little CMS has grown to become one of the most popular open-source color management libraries, and has been used in a large number of production projects, in areas as printer firmware, monitors, digital cameras, RIPs, publishing, scientific, and many others. You can find Little CMS in most Linux distributions, and it's released under an open source license. 

### Please see the complete documentation in doc folder

# Reverse CLUT index
A single float 3→3 CLUT pipeline using the default tetrahedral interpolation can be indexed for full-branch reverse lookups. Given one output color, `cmsReverseCLUTQuery` returns every distinct input in the closed cube `[0,1]^3` that maps to it; folded LUTs therefore report all branches instead of a single hint-based result. Build the index once and run as many read-only queries as needed.

```c
cmsPipeline* lut = /* one float 3->3 tetrahedral CLUT stage */;
cmsReverseCLUT* index = cmsReverseCLUTAlloc(lut);
if (index == NULL) { /* unsupported pipeline, see cmsSignalError */ }

cmsFloat32Number target[3] = { 0.4f, 0.3f, 0.4f };
cmsUInt32Number count;
cmsBool singular;

/* First pass: count the roots without allocating any output storage. */
if (cmsReverseCLUTQuery(index, target, NULL, 0, &count, &singular, NULL)) {

    cmsFloat32Number (*roots)[3] = malloc(count * sizeof(*roots));

    /* Second pass: fetch the roots, already sorted lexicographically. */
    if (cmsReverseCLUTQuery(index, target, roots, count, &count,
                            &singular, NULL)) {
        for (cmsUInt32Number i = 0; i < count; i++)
            printf("root %u: %g %g %g\n", i, roots[i][0], roots[i][1], roots[i][2]);
        /* count == 0 means the target is unreachable; singular flags degenerate
           tetrahedra encountered near the target, which never add roots themselves. */
    }
    free(roots);
}

cmsReverseCLUTFree(index);   /* cmsReverseCLUTFree(NULL) is safe */
```
