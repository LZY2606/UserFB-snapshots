var number = require('as-number')
var vec = require('./vecutil')

var tmp = vec.create()
var capEnd = vec.create()
var lineA = vec.create()
var lineB = vec.create()
var tangent = vec.create()
var miter = vec.create()

var util = require('polyline-miter-util')
var computeMiter = util.computeMiter,
    normal = util.normal,
    direction = util.direction

function Stroke(opt) {
    if (!(this instanceof Stroke))
        return new Stroke(opt)
    opt = opt||{}
    this.miterLimit = number(opt.miterLimit, 10)
    this.thickness = number(opt.thickness, 1)
    this.join = opt.join || 'miter'
    this.cap = opt.cap || 'butt'
    this._normal = null
    this._lastFlip = -1
    this._started = false
}

Stroke.prototype.mapThickness = function(point, i, points) {
    return this.thickness
}

Stroke.prototype.build = function(points) {
    var complex = {
        positions: [],
        cells: []
    }

    if (points.length <= 1)
        return complex

    //collapse runs of identical consecutive points into a single
    //point, keeping the identity (and original index) of the last
    //sample in each run
    var pts = [points[0]]
    var idx = [0]
    for (var i=1; i<points.length; i++) {
        var p = points[i]
        var kept = pts[pts.length-1]
        if (p[0] === kept[0] && p[1] === kept[1]) {
            pts[pts.length-1] = p
            idx[idx.length-1] = i
        } else {
            pts.push(p)
            idx.push(i)
        }
    }

    //fewer than two distinct positions, nothing to extrude
    if (pts.length <= 1)
        return complex

    var total = pts.length

    //query thickness once for each kept sample that ends a segment;
    //the start point borrows the first segment's thickness
    var half = new Array(total)
    for (var i=1; i<total; i++) {
        half[i] = this.mapThickness(pts[i], idx[i], points) / 2
    }
    half[0] = half[1]

    //clear flags
    this._lastFlip = -1
    this._started = false
    this._normal = null

    //join each segment
    for (var i=1, count=0; i<total; i++) {
        var last = pts[i-1]
        var cur = pts[i]
        var next = i<total-1 ? pts[i+1] : null

        if (next && isReversal(last, cur, next)) {
            //the next segment backtracks exactly over this one:
            //end the current stroke at the fold point with a regular
            //cap, then start a fresh stroke from the fold point
            count += this._seg(complex, count, last, cur, null, half[i])
            this._lastFlip = -1
            this._started = false
            this._normal = null
        } else {
            count += this._seg(complex, count, last, cur, next, half[i])
        }
    }
    return complex
}

//are the two non-zero segments meeting at cur exactly opposite?
function isReversal(last, cur, next) {
    var ax = cur[0] - last[0]
    var ay = cur[1] - last[1]
    var bx = next[0] - cur[0]
    var by = next[1] - cur[1]
    var cross = ax * by - ay * bx
    var dot = ax * bx + ay * by
    return cross === 0 && dot < 0
}

Stroke.prototype._seg = function(complex, index, last, cur, next, halfThick) {
    var count = 0
    var cells = complex.cells
    var positions = complex.positions
    var capSquare = this.cap === 'square'
    var joinBevel = this.join === 'bevel'

    //get unit direction of line
    direction(lineA, cur, last)

    //if we don't yet have a normal from previous join,
    //compute based on line start - end
    if (!this._normal) {
        this._normal = vec.create()
        normal(this._normal, lineA)
    }

    //if we haven't started yet, add the first two points
    if (!this._started) {
        this._started = true

        //if the end cap is type square, we can just push the verts out a bit
        if (capSquare) {
            vec.scaleAndAdd(capEnd, last, lineA, -halfThick)
            last = capEnd
        }

        extrusions(positions, last, this._normal, halfThick)
    }

    cells.push([index+0, index+1, index+2])

    /*
    // now determine the type of join with next segment

    - round (TODO)
    - bevel 
    - miter
    - none (i.e. no next segment, use normal)
     */
    
    if (!next) { //no next segment, simple extrusion
        //now reset normal to finish cap
        normal(this._normal, lineA)

        //push square end cap out a bit
        if (capSquare) {
            vec.scaleAndAdd(capEnd, cur, lineA, halfThick)
            cur = capEnd
        }

        extrusions(positions, cur, this._normal, halfThick)
        cells.push(this._lastFlip===1 ? [index, index+2, index+3] : [index+2, index+1, index+3])

        count += 2
     } else { //we have a next segment, start with miter
        //get unit dir of next line
        direction(lineB, next, cur)

        //stores tangent & miter
        var miterLen = computeMiter(tangent, miter, lineA, lineB, halfThick)

        // normal(tmp, lineA)
        
        //get orientation
        var flip = (vec.dot(tangent, this._normal) < 0) ? -1 : 1

        var bevel = joinBevel
        if (!bevel && this.join === 'miter') {
            var limit = miterLen / (halfThick)
            if (limit > this.miterLimit)
                bevel = true
        }

        if (bevel) {    
            //next two points in our first segment
            vec.scaleAndAdd(tmp, cur, this._normal, -halfThick * flip)
            positions.push(vec.clone(tmp))
            vec.scaleAndAdd(tmp, cur, miter, miterLen * flip)
            positions.push(vec.clone(tmp))


            cells.push(this._lastFlip!==-flip
                    ? [index, index+2, index+3] 
                    : [index+2, index+1, index+3])

            //now add the bevel triangle
            cells.push([index+2, index+3, index+4])

            normal(tmp, lineB)
            vec.copy(this._normal, tmp) //store normal for next round

            vec.scaleAndAdd(tmp, cur, tmp, -halfThick*flip)
            positions.push(vec.clone(tmp))

            // //the miter is now the normal for our next join
            count += 3
        } else { //miter
            //next two points for our miter join
            extrusions(positions, cur, miter, miterLen)
            cells.push(this._lastFlip===1
                    ? [index, index+2, index+3] 
                    : [index+2, index+1, index+3])

            flip = -1

            //the miter is now the normal for our next join
            vec.copy(this._normal, miter)
            count += 2
        }
        this._lastFlip = flip
     }
     return count
}

function extrusions(positions, point, normal, scale) {
    //next two points to end our segment
    vec.scaleAndAdd(tmp, point, normal, -scale)
    positions.push(vec.clone(tmp))

    vec.scaleAndAdd(tmp, point, normal, scale)
    positions.push(vec.clone(tmp))
}

module.exports = Stroke