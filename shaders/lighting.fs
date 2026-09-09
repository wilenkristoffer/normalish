#version 330

// Inputs from raylib's default vertex shader.
in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;   // albedo: either the photo or a live webcam frame
uniform vec4 colDiffuse;

uniform vec2 texelSize;       // 1.0 / source texture size, for the Sobel taps
uniform float relief;         // how pronounced the derived relief is
uniform float sampleSpacing;  // tap distance in texels; larger ignores fine detail

// Geometry model: 0 = luminance relief (flat plane, orthographic viewer, the
// original), 1 = a real depth map with a perspective camera. The two use
// different spaces, and lightPos is supplied in whichever one is active.
uniform int geometryMode;

uniform sampler2D depthMap;   // R channel: metres along the view axis, 0 = no data
uniform float tanHalfFovX;    // tan of half the horizontal field of view
uniform float depthSpacing;   // tap distance in depth texels for the gradient
// The depth map's own texel size, which is not the albedo's: the model returns
// 518 wide against a 1280-wide frame. Tapping the gradient at albedo spacing
// samples well inside a single depth texel, where the bilinear interpolation
// is piecewise linear, and the normals then follow the interpolation grid
// rather than the surface.
uniform vec2 depthTexelSize;
// How much of the measured surface tilt to keep, 1 = all of it. Exact depth
// needs none of this, but a monocular model is trained for depth accuracy
// rather than gradient accuracy: its surfaces undulate by centimetres, and at
// room scale one depth texel spans only millimetres of scene, so a small
// depth error becomes a large tilt. Attenuating the tilt trades some real
// shape for normals that are not dominated by that error.
uniform float depthRelief;

// Which pixels are the subject.
//
// The subject is shaded with the measured normal. The room is shaded with a
// flat normal facing the camera instead, which is the point of the split: the
// artefacts that made relighting the whole frame unusable came from the
// model's noisy depth gradients, not from the light itself. With a flat
// normal the room still gets distance falloff and a smooth pool of light, so
// the light stays visible as it moves around the scene, and there is no
// gradient left to go wrong.
uniform sampler2D subjectMask;
uniform int maskEnabled;      // 0 shades everything with measured normals
uniform float roomLight;      // how strongly the light reaches the room, 0 = not at all
uniform vec2 depthRange;      // near/far of the source, for the depth view
// Distance at which falloff is measured, so one falloff setting means the
// same thing whether the scene is a synthetic head at 0.6 m or a real room
// running to 8 m. 1.0 in luminance mode, which leaves that model untouched.
uniform float distanceScale;

uniform int shadowSteps;        // 0 disables the shadow march
uniform float shadowBias;       // metres; below this a surface shadows itself
uniform float shadowThickness;  // metres an occluder is assumed to extend back

uniform vec3 lightPos;   // luminance mode: x in 0..aspect, y in 0..1, z above
                         // surface. depth mode: metres in camera space.
uniform vec3 lightColor;
uniform float lightIntensity;
uniform float falloff;
uniform float ambient;
uniform float specularStrength;
uniform float shininess;
uniform float aspect;
uniform int viewMode;    // 0 = lit, 1 = normals, 2 = albedo, 3 = depth, 4 = mask

out vec4 finalColor;

// Luminance as height: the same approximation the CPU generator used, but
// evaluated per pixel on the GPU so it can keep up with live video.
float heightAt(vec2 uv)
{
    vec3 color = texture(texture0, uv).rgb;
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

// Sobel filter over the luminance, then encode the slopes as a normal.
// Widening sampleSpacing takes the place of the CPU blur: bilinear filtering
// averages as the taps spread out, so fine noise stops dominating the gradient.
vec3 computeNormal(vec2 uv)
{
    vec2 d = texelSize*sampleSpacing;

    float tl = heightAt(uv + vec2(-d.x, -d.y));
    float t  = heightAt(uv + vec2( 0.0, -d.y));
    float tr = heightAt(uv + vec2( d.x, -d.y));
    float l  = heightAt(uv + vec2(-d.x,  0.0));
    float r  = heightAt(uv + vec2( d.x,  0.0));
    float bl = heightAt(uv + vec2(-d.x,  d.y));
    float b  = heightAt(uv + vec2( 0.0,  d.y));
    float br = heightAt(uv + vec2( d.x,  d.y));

    float gx = ((tr + 2.0*r + br) - (tl + 2.0*l + bl))/8.0;
    float gy = ((bl + 2.0*b + br) - (tl + 2.0*t + tr))/8.0;

    return normalize(vec3(-gx*relief, -gy*relief, 1.0));
}

// --- depth model -----------------------------------------------------------
//
// The camera sits at the origin looking down +z, so depth grows away from the
// viewer and a surface facing the camera has a normal pointing at -z. Image
// space has y = 0 at the top, so +y points down in 3D as well.

float depthAt(vec2 uv)
{
    return texture(depthMap, uv).r;
}

// A deliberately blurred depth, for the room's lighting only.
//
// The room is shaded with a flat normal, so its gradient noise cannot reach
// the shading - but the *distance* to the light still comes from the depth,
// and the falloff amplifies noise there badly. Five wide taps keep the large
// structure, which is all the room path needs, and drop the per-pixel wobble.
float smoothDepthAt(vec2 uv)
{
    vec2 d = depthTexelSize*3.0;
    float z = depthAt(uv);
    if (z <= 0.0) return 0.0;

    float sum = z;
    float n = 1.0;
    float taps[4] = float[4](
        depthAt(uv + vec2( d.x, 0.0)),
        depthAt(uv - vec2( d.x, 0.0)),
        depthAt(uv + vec2( 0.0, d.y)),
        depthAt(uv - vec2( 0.0, d.y)));
    for (int i = 0; i < 4; i++)
    {
        if (taps[i] > 0.0) { sum += taps[i]; n += 1.0; }
    }
    return sum/n;
}

// A depth sample back to the 3D point that produced it.
vec3 depthPosition(vec2 uv)
{
    float z = depthAt(uv);
    vec2 ndc = uv*2.0 - 1.0;
    return vec3(ndc.x*tanHalfFovX*z, ndc.y*(tanHalfFovX/aspect)*z, z);
}

// One axis of the surface gradient, as the better of the two one-sided
// differences. Across a silhouette the far neighbour is metres away and would
// dominate a central difference, tilting the normal into nonsense; taking
// whichever side is nearer in depth keeps the estimate on the surface we are
// actually standing on. Invalid neighbours are pushed out of contention.
vec3 axisDelta(vec2 uv, vec3 P, float z, vec2 step)
{
    float zb = depthAt(uv - step);
    float zf = depthAt(uv + step);

    float db = (zb > 0.0) ? abs(z - zb) : 1.0e9;
    float df = (zf > 0.0) ? abs(zf - z) : 1.0e9;
    if ((db > 1.0e8) && (df > 1.0e8)) return vec3(0.0);

    return (df <= db) ? (depthPosition(uv + step) - P)
                      : (P - depthPosition(uv - step));
}

vec3 normalFromDepth(vec2 uv)
{
    float z = depthAt(uv);
    if (z <= 0.0) return vec3(0.0, 0.0, -1.0);

    vec2 d = depthTexelSize*max(depthSpacing, 1.0);
    vec3 P = depthPosition(uv);

    vec3 Px = axisDelta(uv, P, z, vec2(d.x, 0.0));
    vec3 Py = axisDelta(uv, P, z, vec2(0.0, d.y));

    vec3 n = cross(Py, Px);
    float len = length(n);
    if (len <= 1.0e-12) return vec3(0.0, 0.0, -1.0);
    n /= len;

    // Scaling the lateral components and renormalising rotates the normal
    // toward the view axis without changing its direction in the image plane,
    // so shape is softened rather than distorted.
    n.xy *= depthRelief;
    return normalize(n);
}

// The inverse of depthPosition: a camera-space point back to the pixel that
// would have seen it.
vec2 projectToUv(vec3 P)
{
    vec2 ndc = vec2(P.x/(tanHalfFovX*P.z), P.y/((tanHalfFovX/aspect)*P.z));
    return ndc*0.5 + 0.5;
}

// Cheap per-pixel value in 0..1. Without it the march quantises every shadow
// edge to a step length and the result is visibly banded.
float hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx)*0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y)*p3.z);
}

// Walk from the surface toward the light, projecting each step back to screen
// space and asking the depth map whether anything is in front of it. Returns 1
// for lit, 0 for shadowed.
//
// A depth map has no back face, so an occluder is assumed to be a slab of
// shadowThickness rather than solid all the way to infinity. Without that a
// silhouette shadows everything behind it forever; with it, a light between
// the subject and the wall still throws a shadow but distant geometry stops
// being blocked by anything that merely overlaps it on screen.
//
// Only the subject casts. Letting the whole room occlude means a lamp, a
// doorway or a curtain at a completely different depth sweeps a shadow across
// the subject as the light moves, which reads as shadows appearing in the
// room at random. Their own shadows were never visible anyway - the mask
// blends the background back to the original frame - so nothing is lost.
float shadowFactor(vec3 P, vec3 L, float lightDist)
{
    if (shadowSteps <= 0) return 1.0;

    // Cap the march relative to the scene rather than at a fixed distance:
    // four times the nearest surface reaches past the back wall in both a
    // synthetic head scene and a real room, whereas a fixed two metres would
    // cover only the first quarter of a room-scale ray.
    float span = min(lightDist, distanceScale*4.0);
    float stepLen = span/float(shadowSteps);
    float jitter = hash12(gl_FragCoord.xy);

    // Occluded samples are counted rather than returned on immediately. A
    // binary test plus jitter dithers every shadow edge into speckle; counting
    // turns that same jitter into a gradient, which reads as a penumbra and
    // costs nothing but the loss of the early exit.
    float occluded = 0.0;

    for (int i = 0; i < shadowSteps; i++)
    {
        float t = shadowBias + (float(i) + jitter)*stepLen;
        if (t >= lightDist) break;

        vec3 S = P + L*t;
        if (S.z <= 0.0001) break;

        vec2 uv = projectToUv(S);
        // A straight ray projects to a straight line, so once it leaves the
        // frame it will not come back and there is nothing more to test.
        if ((uv.x < 0.0) || (uv.x > 1.0) || (uv.y < 0.0) || (uv.y > 1.0)) break;

        float sceneZ = depthAt(uv);
        if (sceneZ <= 0.0) continue;    // no data is not an occluder
        // Nor is anything outside the subject.
        if ((maskEnabled == 1) && (texture(subjectMask, uv).r < 0.5)) continue;

        float penetration = S.z - sceneZ;
        if ((penetration > shadowBias) && (penetration < shadowThickness)) occluded += 1.0;
    }

    // A quarter of the ray spent inside geometry is treated as fully
    // shadowed; below that the transition is smooth.
    float coverage = occluded/float(shadowSteps);
    return 1.0 - smoothstep(0.0, 0.25, coverage);
}

void main()
{
    vec3 albedo = texture(texture0, fragTexCoord).rgb*colDiffuse.rgb*fragColor.rgb;

    vec3 P;        // surface position, in whichever space the mode uses
    vec3 normal;
    vec3 V;        // surface to viewer

    if (geometryMode == 1)
    {
        P = depthPosition(fragTexCoord);
        normal = normalFromDepth(fragTexCoord);
        // A real perspective viewer, so the specular highlight moves with
        // position rather than sitting dead ahead everywhere.
        V = (P.z > 0.0) ? normalize(-P) : vec3(0.0, 0.0, -1.0);
    }
    else
    {
        // The surface is the z = 0 plane measured in units of its own height,
        // so the light stays circular on a non-square image.
        P = vec3(fragTexCoord.x*aspect, fragTexCoord.y, 0.0);
        normal = computeNormal(fragTexCoord);
        V = vec3(0.0, 0.0, 1.0);   // orthographic viewer looking straight on
    }

    // How much of this pixel is subject. Outside it, swing the normal back to
    // face the camera so the room is lit as though it were a flat surface at
    // its measured depth - depth is reliable enough for distance, and it is
    // the gradient that is not.
    float subject = 1.0;
    if ((geometryMode == 1) && (maskEnabled == 1))
    {
        subject = texture(subjectMask, fragTexCoord).r;
        normal = normalize(mix(vec3(0.0, 0.0, -1.0), normal, subject));

        // And take the room's position from the blurred depth, so the falloff
        // distance is smooth there too.
        float zSmooth = smoothDepthAt(fragTexCoord);
        if (zSmooth > 0.0)
        {
            vec2 ndc = fragTexCoord*2.0 - 1.0;
            float zUse = mix(zSmooth, P.z, subject);
            P = vec3(ndc.x*tanHalfFovX*zUse, ndc.y*(tanHalfFovX/aspect)*zUse, zUse);
        }
    }

    vec3 toLight = lightPos - P;
    float dist = length(toLight);
    vec3 L = toLight/max(dist, 0.0001);
    vec3 H = normalize(L + V);

    // A point light has a singularity at zero distance, and the falloff's
    // derivative is largest there - which is exactly where depth noise turns
    // into speckle, and where placing the light on a surface blows it out.
    // Clamping the distance to a fraction of the scene's near plane bounds
    // both. The direction is taken before the clamp, so the light still
    // points from where it was put.
    float scaled = max(dist, distanceScale*0.25)/max(distanceScale, 0.0001);
    float atten = lightIntensity/(1.0 + falloff*scaled*scaled);

    // With a flat normal, dot(N, L) reduces to (P.z - lightPos.z)/dist, so it
    // passes through exactly zero at the light's depth plane and max(dot, 0)
    // clamps hard there. Any surface sitting near that plane then flickers on
    // and off with the model's depth noise, which is what speckled the room.
    //
    // The room therefore uses a wrapped term that never reaches zero and has
    // no terminator to straddle. It gives up the fact that a light behind a
    // wall should not light it - a distinction resting on the least reliable
    // part of the depth - and keeps distance falloff doing the work of placing
    // the light. The subject keeps true Lambert, where the normals are real.
    float ndl = dot(normal, L);
    float diffuse = mix(ndl*0.5 + 0.5, max(ndl, 0.0), subject)*atten;
    // Diffuse only in the room: a specular highlight on a flat normal is a
    // hard bright disc, which reads as a blemish on a wall rather than as a
    // glint on a surface.
    float specular = pow(max(dot(normal, H), 0.0), shininess)*specularStrength*atten*subject;

    // Only the depth model can cast shadows; the luminance model has no
    // geometry to march through. Ambient is deliberately left out of the
    // shadow term, since a shadowed surface still sees the room.
    if (geometryMode == 1)
    {
        float shadow = shadowFactor(P, L, dist);
        diffuse *= shadow;
        specular *= shadow;
    }

    vec3 color = albedo*(ambient + diffuse*lightColor) + specular*lightColor;

    // The subject is fully relit; the room is blended toward its lit version
    // by roomLight, so 0 leaves the frame exactly as the camera saw it and
    // higher values let the light show in the scene.
    if ((geometryMode == 1) && (maskEnabled == 1))
    {
        color = mix(albedo, color, mix(roomLight, 1.0, subject));
    }

    if (viewMode == 4)
    {
        float subject = texture(subjectMask, fragTexCoord).r;
        color = mix(albedo*0.25, vec3(0.2, 1.0, 0.3), subject*0.75);
    }
    else if (viewMode == 1) color = normal*0.5 + 0.5;
    else if (viewMode == 2) color = albedo;
    else if (viewMode == 3)
    {
        // Near is white, far is black, no data is red so it cannot be
        // mistaken for geometry.
        float z = depthAt(fragTexCoord);
        if (z <= 0.0) color = vec3(0.6, 0.0, 0.0);
        else
        {
            float t = (z - depthRange.x)/max(depthRange.y - depthRange.x, 0.0001);
            color = vec3(1.0 - clamp(t, 0.0, 1.0));
        }
    }

    finalColor = vec4(color, 1.0);
}
