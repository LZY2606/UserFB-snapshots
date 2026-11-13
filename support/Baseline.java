package org.jooq.lambda.support;
import java.util.*;
import java.util.concurrent.atomic.AtomicInteger;
import org.jooq.lambda.Seq;
import org.jooq.lambda.tuple.Tuple2;
public final class Baseline {
 public static void main(String[] args){
  AtomicInteger seen=new AtomicInteger(),closed=new AtomicInteger();
  List<String> ids;
  try(Seq<Tuple2<Integer,String>> source=Seq.of(new Tuple2<>(2,"b"),new Tuple2<>(1,"a"),new Tuple2<>(2,"c"),new Tuple2<>(1,"d")).peek(x->seen.incrementAndGet()).onClose(closed::incrementAndGet)){
   Seq<Tuple2<Integer,String>> sorted=source.sorted(Comparator.comparing((Tuple2<Integer,String> t)->t.v1));
   if(seen.get()!=0)throw new AssertionError("eager");
   ids=sorted.map(t->t.v2).toList();
   if(!ids.equals(Arrays.asList("a","d","b","c"))||seen.get()!=4)throw new AssertionError(ids);
  }
  if(closed.get()!=1)throw new AssertionError("close");
  System.out.println("JOOL_BASELINE stable=a,d,b,c lazy=ok consumed=4 close=1");
 }
}
