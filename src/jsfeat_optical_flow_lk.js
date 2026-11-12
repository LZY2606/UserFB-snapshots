/**
 * @author Eugene Zatepyakin / http://inspirit.ru/
 *
 * this code is a rewrite from OpenCV's Lucas-Kanade optical flow implementation
 */

(function(global) {
    "use strict";
    //
    var optical_flow_lk = (function() {

        // short link to shar deriv
        var scharr_deriv = jsfeat.imgproc.scharr_derivatives;

        // Scharr kernels above sum to 32, so divide to obtain a per-pixel
        // derivative that is consistent with the gray-level residual units.
        var SCHARR_SCALE = 1.0/32.0;

        // Solves a 4x4 symmetric system H*x = rhs with column equilibration
        // and partial pivoting. Returns false when the system is singular or
        // otherwise not solvable in a numerically meaningful way.
        var solve4 = function(Hx, rhs, x) {
            var c0 = Math.sqrt(Hx[0]*Hx[0] + Hx[4]*Hx[4] + Hx[8]*Hx[8] + Hx[12]*Hx[12]);
            var c1 = Math.sqrt(Hx[1]*Hx[1] + Hx[5]*Hx[5] + Hx[9]*Hx[9] + Hx[13]*Hx[13]);
            var c2 = Math.sqrt(Hx[2]*Hx[2] + Hx[6]*Hx[6] + Hx[10]*Hx[10] + Hx[14]*Hx[14]);
            var c3 = Math.sqrt(Hx[3]*Hx[3] + Hx[7]*Hx[7] + Hx[11]*Hx[11] + Hx[15]*Hx[15]);
            var cmax = Math.max(c0, c1, c2, c3);
            if(!(cmax > 0.0) || !isFinite(cmax)) {
                return false;
            }
            var tiny = 1e-12*cmax;
            if(c0 < tiny || c1 < tiny || c2 < tiny || c3 < tiny) {
                return false;
            }

            // A = S*H*S with S = diag(1/c), b = S*rhs
            var A = new Float64Array(16);
            var b = new Float64Array(4);
            var inv = [1.0/c0, 1.0/c1, 1.0/c2, 1.0/c3];
            var i=0, j=0, k=0;
            for(i = 0; i < 4; ++i) {
                b[i] = rhs[i]*inv[i];
                for(j = 0; j < 4; ++j) {
                    A[i*4+j] = Hx[i*4+j]*inv[i]*inv[j];
                }
            }

            var amax = 0.0;
            for(i = 0; i < 16; ++i) {
                var av = Math.abs(A[i]);
                if(av > amax) amax = av;
            }
            if(!(amax > 0.0) || !isFinite(amax)) {
                return false;
            }

            // gaussian elimination with partial pivoting
            for(k = 0; k < 4; ++k) {
                var piv = k, pivval = Math.abs(A[k*4+k]);
                for(i = k+1; i < 4; ++i) {
                    var cand = Math.abs(A[i*4+k]);
                    if(cand > pivval) { piv = i; pivval = cand; }
                }
                if(!(pivval > 1e-10*amax) || !isFinite(pivval)) {
                    return false;
                }
                if(piv != k) {
                    for(j = k; j < 4; ++j) {
                        var t = A[k*4+j]; A[k*4+j] = A[piv*4+j]; A[piv*4+j] = t;
                    }
                    var tb = b[k]; b[k] = b[piv]; b[piv] = tb;
                }
                var pv = A[k*4+k];
                for(i = k+1; i < 4; ++i) {
                    var f = A[i*4+k]/pv;
                    if(f === 0.0) continue;
                    A[i*4+k] = 0.0;
                    for(j = k+1; j < 4; ++j) {
                        A[i*4+j] -= f*A[k*4+j];
                    }
                    b[i] -= f*b[k];
                }
            }

            // back substitution
            for(i = 3; i >= 0; --i) {
                var sum = b[i];
                for(j = i+1; j < 4; ++j) {
                    sum -= A[i*4+j]*x[j];
                }
                var diag = A[i*4+i];
                if(!isFinite(diag) || diag === 0.0) {
                    return false;
                }
                x[i] = sum/diag;
            }

            // undo column scaling, x = y/c
            for(i = 0; i < 4; ++i) {
                x[i] *= inv[i];
                if(!isFinite(x[i])) return false;
            }
            return true;
        };

        // Bilinearly samples a single-channel (U8 image or Float32 derivative)
        // array at integer base (ix,iy) with fractions (fx,fy). The caller
        // must have established that the 2x2 support is usable.
        var sample_bilinear = function(arr, w, ix, iy, fx, fy) {
            var s = iy*w + ix;
            var a = arr[s], b = arr[s+1];
            var c = arr[s+w], d = arr[s+w+1];
            var top = a + (b-a)*fx;
            var bot = c + (d-c)*fx;
            return top + (bot-top)*fy;
        };

        // Fills scaled Scharr derivatives for pixels whose full 3x3 support
        // is inside the image and valid. Pixels without valid support leave
        // vd at 0 and are never sampled.
        var grad_field = function(img, valid, w, h, gx, gy, vd) {
            for(var y = 1; y < h-1; ++y) {
                var row = y*w;
                for(var x = 1; x < w-1; ++x) {
                    var q = row + x;
                    if(valid !== null) {
                        var p0 = (y-1)*w + x, p1 = (y+1)*w + x;
                        if(!(valid[p0-1] && valid[p0] && valid[p0+1] &&
                             valid[ q-1] && valid[ q] && valid[ q+1] &&
                             valid[p1-1] && valid[p1] && valid[p1+1])) {
                            continue;
                        }
                    }
                    var s0 = (y-1)*w, s1 = row, s2 = (y+1)*w;
                    gx[q] = (3.0*(img[s2+x+1] - img[s2+x-1]) +
                             10.0*(img[s1+x+1] - img[s1+x-1]) +
                             3.0*(img[s0+x+1] - img[s0+x-1]))*SCHARR_SCALE;
                    gy[q] = (3.0*(img[s2+x+1] - img[s0+x+1]) +
                             10.0*(img[s2+x] - img[s0+x]) +
                             3.0*(img[s2+x-1] - img[s0+x-1]))*SCHARR_SCALE;
                    vd[q] = 1;
                }
            }
        };
        return {
            track: function(prev_pyr, curr_pyr, prev_xy, curr_xy, count, win_size, max_iter, status, eps, min_eigen_threshold) {
                if (typeof max_iter === "undefined") { max_iter = 30; }
                if (typeof status === "undefined") { status = new Uint8Array(count); }
                if (typeof eps === "undefined") { eps = 0.01; }
                if (typeof min_eigen_threshold === "undefined") { min_eigen_threshold = 0.0001; }

                var half_win = (win_size-1)*0.5;
                var win_area = (win_size*win_size)|0;
                var win_area2 = win_area << 1;
                var prev_imgs = prev_pyr.data, next_imgs = curr_pyr.data;
                var img_prev=prev_imgs[0].data,img_next=next_imgs[0].data;
                var w0 = prev_imgs[0].cols, h0 = prev_imgs[0].rows,lw=0,lh=0;

                var iwin_node = jsfeat.cache.get_buffer(win_area<<2);
                var deriv_iwin_node = jsfeat.cache.get_buffer(win_area2<<2);
                var deriv_lev_node = jsfeat.cache.get_buffer((h0*(w0<<1))<<2);

                var deriv_m = new jsfeat.matrix_t(w0, h0, jsfeat.S32C2_t, deriv_lev_node.data);

                var iwin_buf = iwin_node.i32;
                var deriv_iwin = deriv_iwin_node.i32;
                var deriv_lev = deriv_lev_node.i32;

                var dstep=0,src=0,dsrc=0,iptr=0,diptr=0,jptr=0;
                var lev_sc=0.0,prev_x=0.0,prev_y=0.0,next_x=0.0,next_y=0.0;
                var prev_delta_x=0.0,prev_delta_y=0.0,delta_x=0.0,delta_y=0.0;
                var iprev_x=0,iprev_y=0,inext_x=0,inext_y=0;
                var i=0,j=0,x=0,y=0,level=0,ptid=0,iter=0;
                var brd_tl=0,brd_r=0,brd_b=0;
                var a=0.0,b=0.0,b1=0.0,b2=0.0;

                // fixed point math
                var W_BITS14 = 14;
                var W_BITS4 = 14;
                var W_BITS1m5 = W_BITS4 - 5;
                var W_BITS1m51 = (1 << ((W_BITS1m5) - 1));
                var W_BITS14_ = (1 << W_BITS14);
                var W_BITS41 = (1 << ((W_BITS4) - 1));
                var FLT_SCALE = 1.0/(1 << 20);
                var iw00=0,iw01=0,iw10=0,iw11=0,ival=0,ixval=0,iyval=0;
                var A11=0.0,A12=0.0,A22=0.0,D=0.0,min_eig=0.0;

                var FLT_EPSILON = 0.00000011920929;
                eps *= eps;

                // reset status
                for(; i < count; ++i) {
                    status[i] = 1;
                }

                var max_level = (prev_pyr.levels - 1)|0;
                level = max_level;

                for(; level >= 0; --level) {
                    lev_sc = (1.0/(1 << level));
                    lw = w0 >> level;
                    lh = h0 >> level;
                    dstep = lw << 1;
                    img_prev = prev_imgs[level].data;
                    img_next = next_imgs[level].data;
                    
                    brd_r = (lw - win_size)|0;
                    brd_b = (lh - win_size)|0;

                    // calculate level derivatives
                    scharr_deriv(prev_imgs[level], deriv_m);

                    // iterate through points
                    for(ptid = 0; ptid < count; ++ptid) {
                        i = ptid << 1;
                        j = i + 1;
                        prev_x = prev_xy[i]*lev_sc;
                        prev_y = prev_xy[j]*lev_sc;

                        if( level == max_level ) {
                            next_x = prev_x;
                            next_y = prev_y;
                        } else {
                            next_x = curr_xy[i]*2.0;
                            next_y = curr_xy[j]*2.0;
                        }
                        curr_xy[i] = next_x;
                        curr_xy[j] = next_y;

                        prev_x -= half_win;
                        prev_y -= half_win;
                        iprev_x = prev_x|0;
                        iprev_y = prev_y|0;

                        // border check
                        x = (iprev_x <= brd_tl)|(iprev_x >= brd_r)|(iprev_y <= brd_tl)|(iprev_y >= brd_b);
                        if( x != 0 ) {
                            if( level == 0 ) {
                                status[ptid] = 0;
                            }
                            continue;
                        }

                        a = prev_x - iprev_x;
                        b = prev_y - iprev_y;
                        iw00 = (((1.0 - a)*(1.0 - b)*W_BITS14_) + 0.5)|0;
                        iw01 = ((a*(1.0 - b)*W_BITS14_) + 0.5)|0;
                        iw10 = (((1.0 - a)*b*W_BITS14_) + 0.5)|0;
                        iw11 = (W_BITS14_ - iw00 - iw01 - iw10);

                        A11 = 0.0, A12 = 0.0, A22 = 0.0;

                        // extract the patch from the first image, compute covariation matrix of derivatives
                        for( y = 0; y < win_size; ++y ) {
                            src = ( (y + iprev_y)*lw + iprev_x )|0;
                            dsrc = src << 1;

                            iptr = (y*win_size)|0;
                            diptr = iptr << 1;
                            for(x = 0 ; x < win_size; ++x, ++src, ++iptr, dsrc += 2) {
                                ival = ( (img_prev[src])*iw00 + (img_prev[src+1])*iw01 +
                                        (img_prev[src+lw])*iw10 + (img_prev[src+lw+1])*iw11 );
                                ival = (((ival) + W_BITS1m51) >> (W_BITS1m5));

                                ixval = ( deriv_lev[dsrc]*iw00 + deriv_lev[dsrc+2]*iw01 +
                                        deriv_lev[dsrc+dstep]*iw10 + deriv_lev[dsrc+dstep+2]*iw11 );
                                ixval = (((ixval) + W_BITS41) >> (W_BITS4));

                                iyval = ( deriv_lev[dsrc+1]*iw00 + deriv_lev[dsrc+3]*iw01 + deriv_lev[dsrc+dstep+1]*iw10 +
                                        deriv_lev[dsrc+dstep+3]*iw11 );
                                iyval = (((iyval) + W_BITS41) >> (W_BITS4));

                                iwin_buf[iptr] = ival;
                                deriv_iwin[diptr++] = ixval;
                                deriv_iwin[diptr++] = iyval;

                                A11 += ixval*ixval;
                                A12 += ixval*iyval;
                                A22 += iyval*iyval;
                            }
                        }

                        A11 *= FLT_SCALE; A12 *= FLT_SCALE; A22 *= FLT_SCALE;

                        D = A11*A22 - A12*A12;
                        min_eig = (A22 + A11 - Math.sqrt((A11-A22)*(A11-A22) + 4.0*A12*A12)) / win_area2;

                        if( min_eig < min_eigen_threshold || D < FLT_EPSILON )
                        {
                            if( level == 0 ) {
                                status[ptid] = 0;
                            }
                            continue;
                        }

                        D = 1.0/D;

                        next_x -= half_win;
                        next_y -= half_win;
                        prev_delta_x = 0.0;
                        prev_delta_y = 0.0;

                        for( iter = 0; iter < max_iter; ++iter ) {
                            inext_x = next_x|0;
                            inext_y = next_y|0;

                            x = (inext_x <= brd_tl)|(inext_x >= brd_r)|(inext_y <= brd_tl)|(inext_y >= brd_b);
                            if( x != 0 ) {
                                if( level == 0 ) {
                                    status[ptid] = 0;
                                }
                                break;
                            }

                            a = next_x - inext_x;
                            b = next_y - inext_y;
                            iw00 = (((1.0 - a)*(1.0 - b)*W_BITS14_) + 0.5)|0;
                            iw01 = ((a*(1.0 - b)*W_BITS14_) + 0.5)|0;
                            iw10 = (((1.0 - a)*b*W_BITS14_) + 0.5)|0;
                            iw11 = (W_BITS14_ - iw00 - iw01 - iw10);
                            b1 = 0.0, b2 = 0.0;

                            for( y = 0; y < win_size; ++y ) {
                                jptr = ( (y + inext_y)*lw + inext_x )|0;

                                iptr = (y*win_size)|0;
                                diptr = iptr << 1;
                                for( x = 0 ; x < win_size; ++x, ++jptr, ++iptr ) {
                                    ival = ( (img_next[jptr])*iw00 + (img_next[jptr+1])*iw01 +
                                            (img_next[jptr+lw])*iw10 + (img_next[jptr+lw+1])*iw11 );
                                    ival = (((ival) + W_BITS1m51) >> (W_BITS1m5));
                                    ival = (ival - iwin_buf[iptr]);

                                    b1 += ival * deriv_iwin[diptr++];
                                    b2 += ival * deriv_iwin[diptr++];
                                }
                            }

                            b1 *= FLT_SCALE;
                            b2 *= FLT_SCALE;

                            delta_x = ((A12*b2 - A22*b1) * D);
                            delta_y = ((A12*b1 - A11*b2) * D);

                            next_x += delta_x;
                            next_y += delta_y;
                            curr_xy[i] = next_x + half_win;
                            curr_xy[j] = next_y + half_win;

                            if( delta_x*delta_x + delta_y*delta_y <= eps ) {
                                break;
                            }

                            if( iter > 0 && Math.abs(delta_x + prev_delta_x) < 0.01 &&
                                            Math.abs(delta_y + prev_delta_y) < 0.01 ) {
                                curr_xy[i] -= delta_x*0.5;
                                curr_xy[j] -= delta_y*0.5;
                                break;
                            }

                            prev_delta_x = delta_x;
                            prev_delta_y = delta_y;
                        }
                    } // points loop
                } // levels loop

                jsfeat.cache.put_buffer(iwin_node);
                jsfeat.cache.put_buffer(deriv_iwin_node);
                jsfeat.cache.put_buffer(deriv_lev_node);
            },

            // Jointly estimates translation and a local affine intensity
            // change (curr ~= gain*prev + bias) on masked pyramids produced
            // by pyramid_t.build_masked(). Points are warm-started at their
            // input coordinates with gain=1, bias=0 and refined coarse-to-fine.
            // Only samples whose entire bilinear (and derivative) support is
            // inside the image and marked valid contribute to the normal
            // equations; excluded values never enter the solve.
            track_photometric: function(prev_pyr, curr_pyr, prev_xy, curr_xy, count, win_size, gains, biases, status, options) {
                if (typeof options === "undefined") { options = {}; }
                var max_iter = (options.max_iter === undefined) ? 40 : options.max_iter;
                var eps = (options.eps === undefined) ? 0.001 : options.eps;
                var min_valid_ratio = (options.min_valid_ratio === undefined) ? 0.6 : options.min_valid_ratio;
                var max_rms = (options.max_rms === undefined) ? 3 : options.max_rms;

                // Reject non-finite options with clear failure semantics
                // rather than letting NaN poison every normal equation.
                if(!isFinite(max_iter) || !isFinite(eps) || !isFinite(min_valid_ratio) || !isFinite(max_rms)) {
                    return;
                }

                count = count|0;
                if(count <= 0) {
                    return;
                }

                var half_win = (win_size-1)*0.5;
                var win_area = (win_size*win_size)|0;
                var levels = Math.min(prev_pyr.levels, curr_pyr.levels)|0;
                if(levels <= 0) {
                    return;
                }

                var eps2 = eps*eps;

                // Snapshot the source coordinates. They define the starting
                // guess (gain=1, bias=0) and are what a failed point is
                // restored to. Only the first 2*count entries are ours.
                var orig = new Float64Array(count << 1);
                var p=0, i=0;
                for(p = 0; p < count; ++p) {
                    i = p << 1;
                    orig[i] = prev_xy[i];
                    orig[i+1] = prev_xy[i+1];
                }

                // Per-point state carried across levels. Brightness parameters
                // are deliberately not rescaled when the resolution doubles.
                var px = new Float64Array(count);
                var py = new Float64Array(count);
                var pgain = new Float64Array(count);
                var pbias = new Float64Array(count);
                var ok = new Uint8Array(count);
                var H = new Float64Array(16);
                var rhs = new Float64Array(4);
                var delta = new Float64Array(4);

                var max_level = levels - 1;
                for(p = 0; p < count; ++p) {
                    i = p << 1;
                    var sc0 = 1.0/(1 << max_level);
                    // Start every point at the matching original coordinate:
                    // the target estimate is the source position, gain=1,
                    // bias=0, whatever values the caller put in curr_xy.
                    px[p] = orig[i]*sc0;
                    py[p] = orig[i+1]*sc0;
                    pgain[p] = 1.0;
                    pbias[p] = 0.0;
                    ok[p] = 1;
                }

                for(var level = max_level; level >= 0; --level) {
                    var lev_sc = 1.0/(1 << level);
                    var prev_m = prev_pyr.data[level];
                    var curr_m = curr_pyr.data[level];
                    var pw = prev_m.cols|0, ph = prev_m.rows|0;
                    var cw = curr_m.cols|0, ch = curr_m.rows|0;
                    var prev_img = prev_m.data, curr_img = curr_m.data;

                    var pv_node = (prev_pyr.validity && prev_pyr.validity[level]) ? prev_pyr.validity[level].data : null;
                    var cv_node = (curr_pyr.validity && curr_pyr.validity[level]) ? curr_pyr.validity[level].data : null;

                    // The warp Jacobian only needs the current frame's
                    // derivatives (forward-additive LK). Compute them (Scharr,
                    // /32) only where the full 3x3 support is inside the image
                    // and valid; any other pixel is never sampled.
                    var cgx = new Float32Array(cw*ch);
                    var cgy = new Float32Array(cw*ch);
                    var cvd = new Uint8Array(cw*ch);
                    grad_field(curr_img, cv_node, cw, ch, cgx, cgy, cvd);

                    for(p = 0; p < count; ++p) {
                        if(!ok[p]) {
                            continue;
                        }
                        i = p << 1;

                        // source window stays fixed at the previous location
                        var sx = orig[i]*lev_sc;
                        var sy = orig[i+1]*lev_sc;
                        // target starts from the carried estimate
                        var tx = px[p];
                        var ty = py[p];
                        var g = pgain[p];
                        var b = pbias[p];

                        var failed = 0;

                        for(var iter = 0; iter < max_iter; ++iter) {
                            H.fill(0);
                            rhs.fill(0);
                            var n = 0;

                            for(var wy = 0; wy < win_size; ++wy) {
                                var oy = wy - half_win;
                                for(var wx = 0; wx < win_size; ++wx) {
                                    var ox = wx - half_win;

                                    var spx = sx + ox, spy = sy + oy;
                                    var ipx = Math.floor(spx), ipy = Math.floor(spy);
                                    var fpx = spx - ipx, fpy = spy - ipy;
                                    if(ipx < 0 || ipy < 0 || ipx+1 >= pw || ipy+1 >= ph) continue;
                                    if(pv_node !== null) {
                                        var q = ipy*pw + ipx;
                                        if(!pv_node[q] || !pv_node[q+1] ||
                                           !pv_node[q+pw] || !pv_node[q+pw+1]) continue;
                                    }
                                    var ip = sample_bilinear(prev_img, pw, ipx, ipy, fpx, fpy);

                                    var tpx = tx + ox, tpy = ty + oy;
                                    var itx = Math.floor(tpx), ity = Math.floor(tpy);
                                    var ftx = tpx - itx, fty = tpy - ity;
                                    if(itx < 0 || ity < 0 || itx+1 >= cw || ity+1 >= ch) continue;
                                    if(cv_node !== null) {
                                        var qc = ity*cw + itx;
                                        if(!cv_node[qc] || !cv_node[qc+1] ||
                                           !cv_node[qc+cw] || !cv_node[qc+cw+1]) continue;
                                    }
                                    var qd = ity*cw + itx;
                                    if(!cvd[qd] || !cvd[qd+1] || !cvd[qd+cw] || !cvd[qd+cw+1]) continue;

                                    var ic = sample_bilinear(curr_img, cw, itx, ity, ftx, fty);
                                    var igx = sample_bilinear(cgx, cw, itx, ity, ftx, fty);
                                    var igy = sample_bilinear(cgy, cw, itx, ity, ftx, fty);

                                    var r = ic - (g*ip + b);

                                    H[0] += igx*igx;
                                    H[1] += igx*igy;
                                    H[2] -= igx*ip;
                                    H[3] -= igx;
                                    H[5] += igy*igy;
                                    H[6] -= igy*ip;
                                    H[7] -= igy;
                                    H[10] += ip*ip;
                                    H[11] += ip;
                                    H[15] += 1.0;

                                    rhs[0] -= igx*r;
                                    rhs[1] -= igy*r;
                                    rhs[2] += ip*r;
                                    rhs[3] += r;

                                    ++n;
                                }
                            }

                            // too few usable samples for a meaningful solve
                            var ratio = n/(win_area|0);
                            if(n === 0 || ratio < min_valid_ratio) {
                                if(level === 0) failed = 1;
                                break;
                            }

                            // mirror symmetric entries
                            H[4] = H[1]; H[8] = H[2]; H[9] = H[6];
                            H[12] = H[3]; H[13] = H[7]; H[14] = H[11];

                            if(!solve4(H, rhs, delta)) {
                                if(level === 0) failed = 1;
                                break;
                            }

                            var dx = delta[0], dy = delta[1], dg = delta[2], db = delta[3];
                            if(!isFinite(dx) || !isFinite(dy) || !isFinite(dg) || !isFinite(db)) {
                                if(level === 0) failed = 1;
                                break;
                            }

                            tx += dx; ty += dy; g += dg; b += db;
                            if(!isFinite(tx) || !isFinite(ty) || !isFinite(g) || !isFinite(b)) {
                                if(level === 0) failed = 1;
                                break;
                            }

                            if(dx*dx + dy*dy <= eps2) {
                                break;
                            }
                        }

                        if(failed) {
                            ok[p] = 0;
                            continue;
                        }

                        if(level === 0) {
                            // Final decision is made at full resolution using
                            // the achieved estimate: enough support and a
                            // residual RMS in gray levels.
                            var sum_r2 = 0.0, fn = 0;
                            for(var fy2 = 0; fy2 < win_size; ++fy2) {
                                var oy2 = fy2 - half_win;
                                for(var fx2 = 0; fx2 < win_size; ++fx2) {
                                    var ox2 = fx2 - half_win;
                                    var spx2 = sx + ox2, spy2 = sy + oy2;
                                    var ipx2 = Math.floor(spx2), ipy2 = Math.floor(spy2);
                                    var fpx2 = spx2 - ipx2, fpy2 = spy2 - ipy2;
                                    if(ipx2 < 0 || ipy2 < 0 || ipx2+1 >= pw || ipy2+1 >= ph) continue;
                                    if(pv_node !== null) {
                                        var q2 = ipy2*pw + ipx2;
                                        if(!pv_node[q2] || !pv_node[q2+1] ||
                                           !pv_node[q2+pw] || !pv_node[q2+pw+1]) continue;
                                    }
                                    var ip2 = sample_bilinear(prev_img, pw, ipx2, ipy2, fpx2, fpy2);

                                    var tpx2 = tx + ox2, tpy2 = ty + oy2;
                                    var itx2 = Math.floor(tpx2), ity2 = Math.floor(tpy2);
                                    var ftx2 = tpx2 - itx2, fty2 = tpy2 - ity2;
                                    if(itx2 < 0 || ity2 < 0 || itx2+1 >= cw || ity2+1 >= ch) continue;
                                    if(cv_node !== null) {
                                        var qc2 = ity2*cw + itx2;
                                        if(!cv_node[qc2] || !cv_node[qc2+1] ||
                                           !cv_node[qc2+cw] || !cv_node[qc2+cw+1]) continue;
                                    }
                                    // derivative support is not needed for the
                                    // residual, only for the Jacobian
                                    var ic2 = sample_bilinear(curr_img, cw, itx2, ity2, ftx2, fty2);

                                    var r2 = ic2 - (g*ip2 + b);
                                    sum_r2 += r2*r2;
                                    ++fn;
                                }
                            }

                            if(fn === 0 || (fn/(win_area|0)) < min_valid_ratio) {
                                ok[p] = 0;
                                continue;
                            }
                            var rms = Math.sqrt(sum_r2/fn);
                            if(!isFinite(rms) || rms > max_rms) {
                                ok[p] = 0;
                                continue;
                            }
                            if(!isFinite(tx) || !isFinite(ty) || !isFinite(g) || !isFinite(b)) {
                                ok[p] = 0;
                                continue;
                            }

                            // success: write original-resolution outputs
                            i = p << 1;
                            curr_xy[i] = tx;
                            curr_xy[i+1] = ty;
                            gains[p] = g;
                            biases[p] = b;
                            status[p] = 1;
                            px[p] = tx; py[p] = ty;
                        } else {
                            // carry to the next finer level
                            px[p] = tx*2.0;
                            py[p] = ty*2.0;
                            pgain[p] = g;
                            pbias[p] = b;
                        }
                    } // points
                } // levels

                // Any point not marked ok keeps its original coordinates and
                // the identity intensity mapping.
                for(p = 0; p < count; ++p) {
                    if(!ok[p]) {
                        i = p << 1;
                        curr_xy[i] = orig[i];
                        curr_xy[i+1] = orig[i+1];
                        gains[p] = 1.0;
                        biases[p] = 0.0;
                        status[p] = 0;
                    }
                }
            }
        };
    })();

    global.optical_flow_lk = optical_flow_lk;

})(jsfeat);
