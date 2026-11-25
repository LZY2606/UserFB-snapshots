use rpt::*;
fn main() {
    let mut scene=Scene::new();
    scene.add(Object::new(Plane{normal:glm::vec3(0.,0.,1.),value:0.}).material(Material::diffuse(glm::vec3(0.5,0.6,0.7))));
    scene.add(Light::Point(glm::vec3(4.,3.,2.),glm::vec3(0.,0.,2.)));
    let camera=Camera::look_at(glm::vec3(0.,0.,2.),glm::vec3(0.,0.,0.),glm::vec3(0.,1.,0.),1.);
    let image=Renderer::new(&scene,camera).width(3).height(3).num_samples(4).max_bounces(0).render();
    let sum:u64=image.pixels().map(|p|p.0.iter().map(|x|u64::from(*x)).sum::<u64>()).sum();
    assert!(sum>0); assert_eq!(image.width()*image.height(),9);
    println!("RPT_BASELINE pixels=9 nonblack=true sum={}",sum);
    let light=Light::Point(glm::vec3(4.,3.,2.),glm::vec3(0.,0.,2.));
    // Production light query uses its real distance law and material evaluation.
    let mut rng=__rng();
    let (i,d,l)=light.illuminate(&glm::vec3(0.,0.,0.),&mut rng);
    assert!((i.x-1.).abs()<1e-12 && (l-2.).abs()<1e-12 && (d.z-1.).abs()<1e-12);
    println!("POINT_BASELINE distance={} intensity_r={} dir_z={}",l,i.x,d.z);
}
fn __rng()->rand::rngs::StdRng { use rand::SeedableRng;rand::rngs::StdRng::seed_from_u64(7) }
