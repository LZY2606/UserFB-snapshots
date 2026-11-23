const cases=[];
export function taskCase(name,fn){if(typeof name!=='string'||typeof fn!=='function')throw Error('invalid taskCase');cases.push({name,fn});}
export async function run(){
 const groups=['clock','cascade','output'];let total=0;
 if(!cases.length)throw Error('zero tests');
 for(const group of groups){
  let n=0,count=0;
  for(const c of cases.filter(x=>x.name===group)){
   let checks=0;
   await c.fn((value,message='check failed')=>{++checks;if(!value)throw Error(message)});
   if(!checks)throw Error('zero assertions: '+group);
   ++n;count+=checks;++total;
  }
  if(!n)throw Error('missing group: '+group);
  console.log('GROUP '+group+' cases='+n+' checks='+count+' COMPLETE');
 }
 if(total!==cases.length)throw Error('unknown group');
 console.log('TARGET_COMPLETE cases='+total);
}
