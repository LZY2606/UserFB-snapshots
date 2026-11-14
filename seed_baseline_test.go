package fauxgl
import ("testing"; "fmt")
func seedTriangle() *Triangle {
    return NewTriangleForPoints(Vector{-.8,-.7,0},Vector{.7,-.6,0},Vector{-.1,.8,0})
}
func TestSeedSolid(t *testing.T) {
    dc:=NewContext(24,20); dc.Cull=CullNone
    dc.Shader=NewSolidColorShader(Identity(),Color{1,.25,.5,1})
    info:=dc.DrawTriangle(seedTriangle())
    c:=dc.ColorBuffer.NRGBAAt(12,10)
    if info.UpdatedPixels==0 || c.R!=255 || c.G!=63 || c.B!=127 || c.A!=255 {t.Fatalf("solid: %+v %+v",c,info)}
    fmt.Printf("FAUXGL_BASELINE_SOLID rgba=%d,%d,%d,%d depth=%.3f\n",c.R,c.G,c.B,c.A,dc.DepthBuffer[10*24+12])
}
func TestSeedPhongClamps(t *testing.T) {
    dc:=NewContext(24,20);dc.Cull=CullNone
    s:=NewPhongShader(Identity(),Vector{0,0,1},Vector{0,0,2})
    s.ObjectColor=Color{4,2,.5,1};s.AmbientColor=White;s.DiffuseColor=Black;s.SpecularPower=0
    dc.Shader=s;info:=dc.DrawTriangle(seedTriangle());c:=dc.ColorBuffer.NRGBAAt(12,10)
    raw:=s.Fragment(Vertex{Position:Vector{},Normal:Vector{0,0,1}})
    if info.UpdatedPixels==0 || c.R!=255 || c.G!=255 || c.B!=127 || raw.R!=1 || raw.G!=1 {t.Fatalf("phong: %+v %+v",c,raw)}
    fmt.Printf("FAUXGL_BASELINE_PHONG shader_rgb=%.1f,%.1f,%.1f rgba=%d,%d,%d,%d\n",raw.R,raw.G,raw.B,c.R,c.G,c.B,c.A)
}
