// Statistical tests for group comparison
// Ported from packages/mapper/visualize.lsh

'use strict';

// ============================================================
// Math helpers
// ============================================================

function lgamma(x) {
  const c = [76.18009172947146,-86.50532032941677,24.01409824083091,
    -1.231739572450155,0.1208650973866179e-2,-0.5395239384953e-5];
  let y = x, tmp = x + 5.5; tmp -= (x+0.5)*Math.log(tmp);
  let ser = 1.000000000190015;
  for(let j=0;j<6;j++) ser += c[j]/++y;
  return -tmp + Math.log(2.5066282746310005*ser/x);
}

function betaCF(a, b, x) {
  const maxIter=200, eps=3e-12;
  const qab=a+b, qap=a+1, qam=a-1;
  let c=1, d=1-qab*x/qap; if(Math.abs(d)<1e-30) d=1e-30; d=1/d;
  let h=d;
  for(let m=1;m<=maxIter;m++) {
    let m2=2*m;
    let aa=m*(b-m)*x/((qam+m2)*(a+m2));
    d=1+aa*d; if(Math.abs(d)<1e-30) d=1e-30; c=1+aa/c; if(Math.abs(c)<1e-30) c=1e-30;
    d=1/d; h*=d*c;
    aa=-(a+m)*(qab+m)*x/((a+m2)*(qap+m2));
    d=1+aa*d; if(Math.abs(d)<1e-30) d=1e-30; c=1+aa/c; if(Math.abs(c)<1e-30) c=1e-30;
    d=1/d; let del=d*c; h*=del;
    if(Math.abs(del-1)<eps) break;
  }
  return h;
}

function regIncBeta(a, b, x) {
  if(x<=0) return 0; if(x>=1) return 1;
  const bt = Math.exp(lgamma(a+b)-lgamma(a)-lgamma(b)+a*Math.log(x)+b*Math.log(1-x));
  if(x < (a+1)/(a+b+2)) return bt*betaCF(a,b,x)/a;
  return 1 - bt*betaCF(b,a,1-x)/b;
}

function tCDF(t, df) {
  const x = df/(df+t*t);
  return 1 - 0.5*regIncBeta(df/2, 0.5, x);
}

function normalCDF(z) {
  const a1=0.254829592,a2=-0.284496736,a3=1.421413741,a4=-1.453152027,a5=1.061405429,p=0.3275911;
  const sign = z<0 ? -1 : 1, x = Math.abs(z)/Math.sqrt(2);
  const t=1/(1+p*x), y=1-((((a5*t+a4)*t+a3)*t+a2)*t+a1)*t*Math.exp(-x*x);
  return 0.5*(1+sign*y);
}

function lnChoose(n, k) {
  if(k<0||k>n) return -Infinity;
  return lgamma(n+1)-lgamma(k+1)-lgamma(n-k+1);
}

function mean(arr) { let s=0; for(let i=0;i<arr.length;i++) s+=arr[i]; return s/arr.length; }
function variance(arr) { const m=mean(arr); let s=0; for(let i=0;i<arr.length;i++) { const d=arr[i]-m; s+=d*d; } return s/(arr.length-1); }

// ============================================================
// Statistical tests
// ============================================================

function welchTTest(xArr, yArr) {
  const n1=xArr.length, n2=yArr.length;
  if(n1<2||n2<2) return {t:0,df:1,p:1,meanA:mean(xArr),meanB:mean(yArr),diff:0};
  const m1=mean(xArr), m2=mean(yArr), v1=variance(xArr), v2=variance(yArr);
  const se=Math.sqrt(v1/n1+v2/n2);
  if(se===0) return {t:0,df:n1+n2-2,p:m1===m2?1:0,meanA:m1,meanB:m2,diff:m1-m2};
  const t=(m1-m2)/se;
  const num=(v1/n1+v2/n2)**2;
  const den=(v1/n1)**2/(n1-1)+(v2/n2)**2/(n2-1);
  const df=num/den;
  const p=2*(1-tCDF(Math.abs(t),df));
  return {t,df,p,meanA:m1,meanB:m2,diff:m1-m2};
}

function mannWhitneyU(xArr, yArr) {
  const n1=xArr.length, n2=yArr.length;
  if(n1<1||n2<1) return {U:0,z:0,p:1,meanA:mean(xArr||[0]),meanB:mean(yArr||[0]),diff:0};
  const all=[];
  for(let i=0;i<n1;i++) all.push({v:xArr[i],g:0});
  for(let i=0;i<n2;i++) all.push({v:yArr[i],g:1});
  all.sort((a,b)=>a.v-b.v);
  const ranks=new Array(all.length);
  let i=0;
  while(i<all.length) {
    let j=i; while(j<all.length&&all[j].v===all[i].v) j++;
    const avgRank=(i+1+j)/2;
    for(let k=i;k<j;k++) ranks[k]=avgRank;
    i=j;
  }
  let R1=0; for(let i=0;i<all.length;i++) if(all[i].g===0) R1+=ranks[i];
  const U1=R1-n1*(n1+1)/2;
  const mU=n1*n2/2;
  const sU=Math.sqrt(n1*n2*(n1+n2+1)/12);
  if(sU===0) return {U:U1,z:0,p:1,meanA:mean(xArr),meanB:mean(yArr),diff:mean(xArr)-mean(yArr)};
  const z=(U1-mU)/sU;
  const p=2*(1-normalCDF(Math.abs(z)));
  return {U:U1,z,p,meanA:mean(xArr),meanB:mean(yArr),diff:mean(xArr)-mean(yArr)};
}

function ksTest(xArr, yArr) {
  const n1=xArr.length, n2=yArr.length;
  if(n1<1||n2<1) return {D:0,p:1,meanA:0,meanB:0,diff:0};
  const s1=[...xArr].sort((a,b)=>a-b), s2=[...yArr].sort((a,b)=>a-b);
  let i=0,j=0,D=0;
  while(i<n1||j<n2) {
    const v1=i<n1?s1[i]:Infinity, v2=j<n2?s2[j]:Infinity;
    if(v1<=v2) i++; if(v2<=v1) j++;
    const diff=Math.abs(i/n1-j/n2);
    if(diff>D) D=diff;
  }
  const en=Math.sqrt(n1*n2/(n1+n2));
  const s=(en+0.12+0.11/en)*D;
  let p=0;
  for(let k=1;k<=100;k++) { p+=2*((k%2===1?1:-1))*Math.exp(-2*k*k*s*s); }
  p=Math.min(1,Math.max(0,p));
  return {D,p,meanA:mean(xArr),meanB:mean(yArr),diff:mean(xArr)-mean(yArr)};
}

function hypergeometricTest(xArr, yArr) {
  const allVals=[...xArr,...yArr];
  const cats=[...new Set(allVals)];
  if(cats.length>20||cats.length<2) return {category:'',p:1,freqA:{},freqB:{}};
  const N=allVals.length, nA=xArr.length, nB=yArr.length;
  let minP=1, bestCat='';
  const freqA={}, freqB={};
  cats.forEach(c => {
    const kA=xArr.filter(v=>v===c).length, kB=yArr.filter(v=>v===c).length;
    freqA[c]=kA; freqB[c]=kB;
    const K=kA+kB;
    let pVal=0;
    for(let x=kA;x<=Math.min(nA,K);x++) {
      pVal+=Math.exp(lnChoose(K,x)+lnChoose(N-K,nA-x)-lnChoose(N,nA));
    }
    if(pVal<minP) { minP=pVal; bestCat=c; }
  });
  minP=Math.min(1,minP*cats.length);
  return {category:bestCat,p:minP,freqA,freqB,meanA:0,meanB:0,diff:0};
}

// ============================================================
// FDR correction
// ============================================================

function benjaminiHochberg(results) {
  const n=results.length;
  const sorted=[...results].sort((a,b)=>a.p-b.p);
  for(let i=0;i<n;i++) sorted[i].q=Math.min(1, sorted[i].p*n/(i+1));
  for(let i=n-2;i>=0;i--) sorted[i].q=Math.min(sorted[i].q, sorted[i+1].q);
  return sorted;
}

// ============================================================
// Column type detection
// ============================================================

function isNumericColumn(values) {
  return values.every(v => !isNaN(Number(v)));
}

function isIntegerColumn(values) {
  return values.every(v => {
    const n = Number(v);
    return !isNaN(n) && Number.isInteger(n);
  });
}

function isCategoricalColumn(values) {
  // Categorical: non-numeric text, or integers with limited distinct values
  if (!isNumericColumn(values)) return true;
  if (isIntegerColumn(values)) {
    const distinct = new Set(values.map(Number));
    return distinct.size <= 20;
  }
  return false;
}

function isCategorical(values) {
  const vals = new Set(values);
  return vals.size <= 20;
}
