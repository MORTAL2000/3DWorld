// 3D World - OpenGL CS184 Computer Graphics Project
// by Frank Gennari
// 9/27/02

#include "3DWorld.h"
#include "mesh.h"
#include "textures_3dw.h"
#include "gl_ext_arb.h"
#include "shaders.h"
#include "draw_utils.h"


float const W_TEX_SCALE0     = 1.0;
float const WATER_WIND_EFF   = 0.0006;
float const START_OFFSET0    = 0.05;
float const PRESP_ANGLE_ADJ  = 1.5;
float const VD_SCALE         = 1.0;
float const SURF_HEAL_RATE   = 0.005;
float const MAX_SURFD        = 20.0;
float const DLIGHT_SCALE     = 4.0;
int   const DRAW_BORDER      = 3;

int   const SHOW_MESH_TIME   = 0;
int   const SHOW_NORMALS     = 0;
int   const DEBUG_COLLS      = 0; // 0 = disabled, 1 = lines, 2 = cubes
int   const DISABLE_TEXTURES = 0;


struct fp_ratio {
	float n, d;
	inline float get_val() const {return ((d == 0.0) ? 1.0 : n/d);}
};


// Global Variables
bool clear_landscape_vbo;
int island(0);
float lt_green_int(1.0), sm_green_int(1.0), water_xoff(0.0), water_yoff(0.0), wave_time(0.0);
vector<fp_ratio> uw_mesh_lighting; // for water caustics

extern bool using_lightmap, has_dl_sources, combined_gu, has_snow, draw_mesh_shader, disable_shaders;
extern int draw_model, num_local_minima, world_mode, xoff, yoff, xoff2, yoff2, ocean_set, ground_effects_level, animate2;
extern int display_mode, frame_counter, resolution, verbose_mode, DISABLE_WATER, read_landscape, disable_inf_terrain;
extern float zmax, zmin, zmax_est, ztop, zbottom, light_factor, max_water_height, init_temperature, univ_temp;
extern float water_plane_z, temperature, fticks, mesh_scale, mesh_z_cutoff, TWO_XSS, TWO_YSS, XY_SCENE_SIZE;
extern point light_pos, litning_pos, sun_pos, moon_pos;
extern vector3d up_norm, wind;
extern colorRGBA bkg_color;
extern float h_dirt[];


void draw_sides_and_bottom();



float camera_min_dist_to_surface() { // min dist of four corners and center

	point pos;
	get_matrix_point(0, 0, pos);
	float dist(distance_to_camera(pos));
	get_matrix_point(MESH_X_SIZE-1, 0, pos);
	dist = min(dist, distance_to_camera(pos));
	get_matrix_point(0, MESH_Y_SIZE-1, pos);
	dist = min(dist, distance_to_camera(pos));
	get_matrix_point(MESH_X_SIZE-1, MESH_Y_SIZE-1, pos);
	dist = min(dist, distance_to_camera(pos));
	get_matrix_point(MESH_X_SIZE/2, MESH_Y_SIZE/2, pos);
	dist = min(dist, distance_to_camera(pos));
	return dist;
}


colorRGBA setup_mesh_lighting() {

	colorRGBA ambient_color(DEF_AMBIENT, DEF_AMBIENT, DEF_AMBIENT, 1.0);
	colorRGBA diffuse_color(DEF_DIFFUSE, DEF_DIFFUSE, DEF_DIFFUSE, 1.0);
	glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, &diffuse_color.R);
	glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, &ambient_color.R);
	glEnable(GL_COLOR_MATERIAL);
	diffuse_color.do_glColor();
	set_fill_mode();
	enable_blend();
	return diffuse_color;
}


void run_post_mesh_draw() {
	
	glEnable(GL_NORMALIZE);
	glDisable(GL_COLOR_MATERIAL);
	disable_blend();
}


float integrate_water_dist(point const &targ_pos, point const &src_pos, float const water_z) {

	if (src_pos.z == targ_pos.z) return 0.0;
	float const t(min(1.0f, (water_z - targ_pos.z)/fabs(src_pos.z - targ_pos.z))); // min(1.0,...) for underwater case
	point p_int(targ_pos + (src_pos - targ_pos)*t);
	int const xp(get_xpos(targ_pos.x)), yp(get_ypos(targ_pos.y));
	if (world_mode == WMODE_GROUND && !point_outside_mesh(xp, yp)) p_int.z = min(src_pos.z, water_matrix[yp][xp]); // account for ripples
	return p2p_dist(p_int, targ_pos)*mesh_scale;
}


void water_color_atten_pt(float *c, int x, int y, point const &pos, point const &p1, point const &p2) {

	float const scale(WATER_COL_ATTEN*((world_mode != WMODE_GROUND || wminside[y][x] == 2) ? 1.0 : 4.0));
	float const wh((world_mode == WMODE_GROUND) ? water_matrix[y][x] : water_plane_z); // higher for interior water
	float const dist(scale*(integrate_water_dist(pos, p1, wh) + integrate_water_dist(pos, p2, wh)));
	atten_by_water_depth(c, dist);
}


float get_cloud_shadow_atten(int x, int y) {

	point const pos(get_xval(x), get_yval(y), mesh_height[y][x]);
	float sval(0.0);
	
	// use the original sun/moon pos - it's wrong, but not too wrong and at least it's independent of the camera pos
	if (light_factor > 0.4) { // sun
		float const cloud_density(get_cloud_density(pos, (sun_pos - pos).get_norm()));
		sval += min(1.0,  5.0*(light_factor - 0.4))*(1.0 - CLIP_TO_01(1.7f*cloud_density));
	}
	if (light_factor < 0.6 && !combined_gu) { // moon
		float const cloud_density(get_cloud_density(pos, (moon_pos - pos).get_norm()));
		sval += min(1.0, -5.0*(light_factor - 0.6))*(1.0 - CLIP_TO_01(1.7f*cloud_density));
	}
	return sval;
};


class mesh_vertex_draw {

	float const healr;
	vector<vert_norm_color> data;

	struct norm_color_ix {
		vector3d n;
		color_wrapper c;
		int ix;
		norm_color_ix() : ix(-1) {}
		norm_color_ix(vert_norm_color const &vnc, int ix_) : n(vnc.n), c(vnc), ix(ix_) {}
	};

	vector<norm_color_ix> last_rows;

	void update_vertex(int i, int j) {
		float color_scale(DEF_DIFFUSE), light_scale(1.0);
		float &sd(surface_damage[i][j]);

		if (sd > 0.0) {
			sd = min(MAX_SURFD, max(0.0f, (sd - healr)));
			color_scale *= max(0.0f, (1.0f - sd));
		}
		//data[c].c.set_to_val(color_scale);
		colorRGB color(color_scale, color_scale, color_scale);

		if (DLIGHT_SCALE > 0.0 && (using_lightmap || has_dl_sources)) { // somewhat slow
			get_sd_light(j, i, get_zpos(data[c].v.z), data[c].v, (!has_dl_sources || draw_mesh_shader), DLIGHT_SCALE, &color.R, &surface_normals[i][j], NULL);
		}
		if (shadow_map_enabled() && draw_mesh_shader) {
			// nothing to do here
		}
		else if (light_factor >= 0.6) { // sun shadows
			light_scale = ((shadow_mask[LIGHT_SUN ][i][j] & SHADOWED_ALL) ? 0.0 : 1.0);
		}
		else if (light_factor <= 0.4) { // moon shadows
			light_scale = ((shadow_mask[LIGHT_MOON][i][j] & SHADOWED_ALL) ? 0.0 : 1.0);
		}
		else { // combined sun and moon shadows
			bool const no_sun ((shadow_mask[LIGHT_SUN ][i][j] & SHADOWED_ALL) != 0);
			bool const no_moon((shadow_mask[LIGHT_MOON][i][j] & SHADOWED_ALL) != 0);
			light_scale = blend_light(light_factor, !no_sun, !no_moon);
		}

		// water light attenuation: total distance from sun/moon, reflected off bottom, to viewer
		if (!DISABLE_WATER && data[c].v.z < max_water_height && data[c].v.z < water_matrix[i][j]) {
			point const pos(get_xval(j), get_yval(i), mesh_height[i][j]);
			water_color_atten(&color.R, j, i, pos);

			if (wminside[i][j] == 1) { // too slow?
				colorRGBA wc(WHITE);
				select_liquid_color(wc, j, i);
				UNROLL_3X(color[i_] *= wc[i_];)
			}
			
			// water caustics: slow and low resolution, but conceptually interesting
			if (light_scale > 0.0 && !uw_mesh_lighting.empty()) {
				float const val(uw_mesh_lighting[i*MESH_X_SIZE + j].get_val());
				//light_scale *= val*val; // square to enhance the caustics effect
				light_scale = pow(val, 8);
			}
		}
		if (ground_effects_level >= 2 && !has_snow && light_scale > 0.0) {
			light_scale *= get_cloud_shadow_atten(j, i);
		}
		// Note: normal is never set to zero because we need it for dynamic light sources
		data[c].n= vertex_normals[i][j]*max(light_scale, 0.01f);
		data[c].set_c3(color);
	}

public:
	unsigned c;

	mesh_vertex_draw() : healr(fticks*SURF_HEAL_RATE), data(2*(MAX_XY_SIZE+1)), c(0) {
		assert(shadow_mask != NULL);
		assert(!data.empty());
		last_rows.resize(MESH_X_SIZE+1);
		data.front().set_state();
	}

	bool draw_mesh_vertex_pair(int i, int j, float x, float y) {
		if (c > 1) {
			if (mesh_draw != NULL && (is_mesh_disabled(j, i) || is_mesh_disabled(j, i+1))) return 0;
			if (mesh_z_cutoff > -FAR_CLIP && mesh_z_cutoff > max(mesh_height[i][j], mesh_height[i+1][j])) return 0;
		}
		for (unsigned p = 0; p < 2; ++p, ++c) {
			int const iinc(min((MESH_Y_SIZE-1), int(i+p)));
			assert(c < data.size());
			data[c].v.assign(x, (y + p*DY_VAL), mesh_height[iinc][j]);
			assert(unsigned(j) < last_rows.size());
		
			if (last_rows[j].ix == iinc) { // gets here nearly half the time
				data[c].n = last_rows[j].n;
				UNROLL_4X(data[c].c[i_] = last_rows[j].c.c[i_];)
			}
			else {
				update_vertex(iinc, j);
				last_rows[j] = norm_color_ix(data[c], iinc);
			}
		}
		return 1;
	}
};


void gen_uw_lighting() {

	uw_mesh_lighting.resize(MESH_X_SIZE*MESH_Y_SIZE);
	point const lpos(get_light_pos());
	float const ssize(X_SCENE_SIZE + Y_SCENE_SIZE + Z_SCENE_SIZE);
	vector<point> rows[2]; // {last, current} y rows
	for (unsigned i = 0; i < 2; ++i) rows[i].resize(MESH_X_SIZE, all_zeros);
	float const dxy_val_inv[2] = {DX_VAL_INV, DY_VAL_INV};

	for (vector<fp_ratio>::iterator i = uw_mesh_lighting.begin(); i != uw_mesh_lighting.end(); ++i) {
		i->n = i->d = 0.0; // initialize
	}
	for (int y = 0; y < MESH_Y_SIZE; ++y) {
		for (int x = 0; x < MESH_X_SIZE; ++x) {
			if (!mesh_is_underwater(x, y)) continue;
			point const p1(get_xval(x), get_yval(y), water_matrix[y][x]); // point on water surface
			vector3d const dir(p1 - lpos);
			vector3d v_refract(dir);
			bool const refracted(calc_refraction_angle(dir, v_refract, wat_vert_normals[y][x], 1.0, WATER_INDEX_REFRACT));
			assert(refracted); // can't have total internal reflection going into the water if the physics are sane
			point const p2(p1 + v_refract.get_norm()*ssize); // distant point along refraction vector
			int xpos(0), ypos(0);
			float zval;

			if (p1.z == p2.z || !line_intersect_mesh(p1, p2, xpos, ypos, zval, 1, 1)) continue; // no intersection
			assert(!point_outside_mesh(xpos, ypos));
			float const t((zval - p1.z)/(p2.z - p1.z));
			point const cpos(p1 + (p2 - p1)*t); // collision point with underwater mesh
			rows[1][x] = cpos;
			if (x == 0 || y == 0) continue; // not an interior point
			if (rows[0][x].z == 0.0 || rows[0][x-1].z == 0.0 || rows[1][x-1].z == 0.0) continue; // incomplete block
			float rng[2][2] = {{cpos.x, cpos.y}, {cpos.x, cpos.y}}; // {min,max} x {x,y} - bounds of mesh surface light patch through this water patch
			int bnds[2][2]; // {min,max} x {x,y} - integer mesh index bounds

			for (unsigned d = 0; d < 2; ++d) { // x,y
				for (unsigned e = 0; e < 2; ++e) { // last,cur y (row)
					for (unsigned f = 0; f < 2; ++f) { // last,cur x
						rng[0][d] = min(rng[0][d], rows[e][x-f][d]);
						rng[1][d] = max(rng[1][d], rows[e][x-f][d]);
					}
				}
				assert(rng[0][d] < rng[1][d]);
				bnds[0][d] = max(0,              int(floor((rng[0][d] + SCENE_SIZE[d])*dxy_val_inv[d])));
				bnds[1][d] = min(MESH_SIZE[d]-1, int(ceil ((rng[1][d] + SCENE_SIZE[d])*dxy_val_inv[d])));
				assert(bnds[0][d] <= bnds[1][d]); // can this fail?
			}
			float const weight_n(1.0/((rng[1][0] - rng[0][0])*(rng[1][1] - rng[0][1]))); // weight of this patch of light
			float const weight_d(1.0/(DX_VAL*DY_VAL));
			float const init_cr[2] = {(-X_SCENE_SIZE + DX_VAL*bnds[0][0]), (-Y_SCENE_SIZE + DY_VAL*bnds[0][1])};
			float crng[2][2]; // {min,max} x {x,y} - range of this mesh quad
			crng[0][1] = init_cr[1];
			crng[1][1] = init_cr[1] + DY_VAL;

			for (int yy = bnds[0][1]; yy < bnds[1][1]; ++yy) {
				assert(yy >= 0 && yy < MESH_Y_SIZE);
				float const ysz(min(crng[1][1], rng[1][1]) - max(crng[0][1], rng[0][1])); // intersection: min(UB) - max(LB)
				assert(ysz >= 0.0);
				if (ysz <= 0.0) continue;
				crng[0][0] = init_cr[0];
				crng[1][0] = init_cr[0] + DX_VAL;

				for (int xx = bnds[0][0]; xx < bnds[1][0]; ++xx) {
					assert(xx >= 0 && xx < MESH_X_SIZE);
					float const xsz(min(crng[1][0], rng[1][0]) - max(crng[0][0], rng[0][0])); // intersection: min(UB) - max(LB)
					assert(xsz >= 0.0);
					if (xsz <= 0.0) continue;
					unsigned const ix(yy*MESH_X_SIZE + xx);
					uw_mesh_lighting[ix].n += weight_n*xsz*ysz; // amount of light through patch of water hitting this mesh quad
					uw_mesh_lighting[ix].d += weight_d*xsz*ysz;
					crng[0][0] += DX_VAL;
					crng[1][0] += DX_VAL;
				}
				crng[0][1] += DY_VAL;
				crng[1][1] += DY_VAL;
			}
		} // for x
		rows[0].swap(rows[1]);
		for (int x = 0; x < MESH_X_SIZE; ++x) rows[1][x] = all_zeros; // reset last row
	} // for y
}


// texture units used: 0: terrain texture, 1: detail texture
void set_landscape_texgen(float tex_scale, int xoffset, int yoffset, int xsize, int ysize, bool use_detail_tex) {

	float const tx(tex_scale*(((float)xoffset)/((float)xsize) + 0.5));
	float const ty(tex_scale*(((float)yoffset)/((float)ysize) + 0.5));
	setup_texgen(tex_scale/TWO_XSS, tex_scale/TWO_YSS, tx, ty);

	if (use_detail_tex) { // blend in detail nose texture at 30x scale
		select_multitex(NOISE_TEX, 1, 0, 0);
		setup_texgen(30.0*tex_scale/TWO_XSS, 30.0*tex_scale/TWO_YSS, 0.0, 0.0);
		set_active_texture(0);
	}
}


void draw_coll_vert(int i, int j) {
	glVertex3f(get_xval(j), get_yval(i), max(czmin, v_collision_matrix[i][j].zmax));
}


void display_mesh() { // fast array version

	if (mesh_height == NULL) return; // no mesh to display
	RESET_TIME;

	if ((display_mode & 0x80) && !DISABLE_WATER && !ocean_set && zmin < max_water_height && ground_effects_level != 0) {
		gen_uw_lighting();
		if (SHOW_MESH_TIME) PRINT_TIME("Underwater Lighting");
	}
	else {
		uw_mesh_lighting.clear();
	}
	if (DEBUG_COLLS) {
		glDisable(GL_LIGHTING);

		if (DEBUG_COLLS == 2) {
			enable_blend();
			colorRGBA(1.0, 0.0, 0.0, 0.1).do_glColor();

			for (int i = 0; i < MESH_Y_SIZE-1; ++i) {
				for (int j = 0; j < MESH_X_SIZE; ++j) {
					if (v_collision_matrix[i][j].zmin < v_collision_matrix[i][j].zmax) {
						point const p1(get_xval(j+0), get_yval(i+0),v_collision_matrix[i][j].zmin);
						point const p2(get_xval(j+1), get_yval(i+1),v_collision_matrix[i][j].zmax);
						draw_cube((p1 + p2)*0.5, (p2.x - p1.x), (p2.y - p1.y), (p2.z - p1.z), 0, 1);
					}
				}
			}
			disable_blend();
		}
		else {
			glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
			BLUE.do_glColor();

			for (int i = 0; i < MESH_Y_SIZE-1; ++i) {
				glBegin(GL_TRIANGLE_STRIP);
				
				for (int j = 0; j < MESH_X_SIZE; ++j) {
					draw_coll_vert(i+0, j);
					draw_coll_vert(i+1, j);
				}
				glEnd();
			}
		}
		glEnable(GL_LIGHTING);
	}
	setup_mesh_lighting();
	update_landscape_texture();
	if (SHOW_MESH_TIME) PRINT_TIME("Landscape Texture");
	glDisable(GL_NORMALIZE);

	if (!DISABLE_TEXTURES) {
		select_texture(LANDSCAPE_TEX);
		set_landscape_texgen(1.0, xoff, yoff, MESH_X_SIZE, MESH_Y_SIZE);
	}
	if (SHOW_MESH_TIME) PRINT_TIME("Preprocess");

	if (ground_effects_level == 0 && setup_gen_buffers()) { // simpler, more efficient mesh draw
		static unsigned mesh_vbo(0);
		
		if (clear_landscape_vbo) {
			delete_vbo(mesh_vbo);
			mesh_vbo = 0;
			clear_landscape_vbo = 0;
		}
		if (mesh_vbo == 0) {
			vector<point> data; // vertex and normals
			data.reserve(4*MESH_X_SIZE*(MESH_Y_SIZE-1));

			for (int i = 0; i < MESH_Y_SIZE-1; ++i) {
				for (int j = 0; j < MESH_X_SIZE; ++j) {
					for (unsigned k = 0; k < 2; ++k) {
						data.push_back(get_mesh_xyz_pos(j, i+k));
						data.push_back(vertex_normals[i+k][j]);
					}
				}
			}
			create_vbo_and_upload(mesh_vbo, data, 0, 0);
		}
		else {
			bind_vbo(mesh_vbo);
		}
		vert_norm::set_vbo_arrays();

		for (int i = 0; i < MESH_Y_SIZE-1; ++i) {
			glDrawArrays(GL_TRIANGLE_STRIP, 2*i*MESH_X_SIZE, 2*MESH_X_SIZE);
		}
		bind_vbo(0);
	}
	else { // slower mesh draw with more features
		shader_t s;

		if (draw_mesh_shader && !disable_shaders) {
			s.set_prefix("#define USE_LIGHT_COLORS", 1); // FS
			s.setup_enabled_lights();
			set_dlights_booleans(s, 1, 1); // FS
			s.set_bool_prefix("use_shadow_map", shadow_map_enabled(), 1); // FS
			s.set_vert_shader("texture_gen.part+draw_mesh");
			s.set_frag_shader("ads_lighting.part*+shadow_map.part*+dynamic_lighting.part*+linear_fog.part+draw_mesh");
			s.begin_shader();
			if (shadow_map_enabled()) set_smap_shader_for_all_lights(s, 0.001);
			s.setup_fog_scale();
			s.add_uniform_int("tex0", 0);
			s.add_uniform_int("tex1", 1);
			s.setup_scene_bounds();
			setup_dlight_textures(s);
		}
		float y(-Y_SCENE_SIZE);
		mesh_vertex_draw mvd;

		for (int i = 0; i < MESH_Y_SIZE-1; ++i) {
			float x(-X_SCENE_SIZE);
			mvd.c = 0;

			for (int j = 0; j < MESH_X_SIZE-1; ++j) {
				if (!mvd.draw_mesh_vertex_pair(i, j, x, y) && mvd.c > 0) {
					glDrawArrays(GL_TRIANGLE_STRIP, 0, mvd.c);
					mvd.c = 0;
				}
				x += DX_VAL;
			} // for j
			mvd.draw_mesh_vertex_pair(i, (MESH_X_SIZE - 1), x, y);
			if (mvd.c > 1) glDrawArrays(GL_TRIANGLE_STRIP, 0, mvd.c);
			y += DY_VAL;
		} // for i
		s.end_shader();
	}
	if (SHOW_MESH_TIME) PRINT_TIME("Draw");
	disable_multitex(1, 1);
	disable_textures_texgen();
	glDisable(GL_COLOR_MATERIAL);
	if (!island) draw_sides_and_bottom();
	run_post_mesh_draw();

	if (SHOW_NORMALS) {
		vector<vert_wrap_t> verts;
		verts.reserve(2*XY_MULT_SIZE);
		set_color(RED);

		for (int i = 1; i < MESH_Y_SIZE-2; ++i) {
			for (int j = 1; j < MESH_X_SIZE-1; ++j) {
				point const pos(get_xval(j), get_yval(i), mesh_height[i][j]);
				verts.push_back(pos);
				verts.push_back(pos + 0.1*vertex_normals[i][j]);
			}
		}
		verts.front().set_state();
		glDrawArrays(GL_LINES, 0, verts.size());
	}
	if (SHOW_MESH_TIME) PRINT_TIME("Final");
}


int set_texture(float zval, int &tex_id) {

	int id;
	for (id = 0; id < NTEX_DIRT-1 && zval >= (h_dirt[id]*2.0*zmax_est - zmax_est); ++id) {}
	update_lttex_ix(id);
	bool const changed(tex_id != lttex_dirt[id].id);
	tex_id = lttex_dirt[id].id;
	return changed;
}


inline void draw_vertex(float x, float y, float z, bool in_y, float tscale=1.0) { // xz or zy

	glTexCoord2f(tscale*(in_y ? z : x), tscale*(in_y ? y : z));
	glVertex3f(x, y, z);
}


// NOTE: There is a buffer of one unit around the drawn area
void draw_sides_and_bottom() {

	int const lx(MESH_X_SIZE-1), ly(MESH_Y_SIZE-1);
	float const botz(zbottom - 0.05), z_avg(0.5*(zbottom + ztop)), ts(4.0/(X_SCENE_SIZE + Y_SCENE_SIZE));
	float const x1(-X_SCENE_SIZE), y1(-Y_SCENE_SIZE), x2(X_SCENE_SIZE-DX_VAL), y2(Y_SCENE_SIZE-DY_VAL);
	int const texture((!read_landscape && get_rel_height(z_avg, zmin, zmax) > lttex_dirt[2].zval) ? ROCK_TEX : DIRT_TEX);
	set_color(WHITE);
	set_lighted_sides(2);
	set_fill_mode();
	
	if (!DISABLE_TEXTURES) select_texture(texture);
	glBegin(GL_QUADS);
	glNormal3f(0.0, 0.0, -1.0); // bottom surface
	draw_one_tquad(x1, y1, x2, y2, botz, 1, ts*x1, ts*y1, ts*x2, ts*y2);
	float xv(x1), yv(y1);
	glNormal3f(0.0, -1.0, 0.0);

	for (int i = 1; i < MESH_X_SIZE; ++i) { // y sides
		for (unsigned d = 0; d < 2; ++d) {
			int const xy_ix(d ? ly : 0);
			float const limit(d ? y2 : y1);
			draw_vertex(xv,        limit, botz, 0, ts);
			draw_vertex(xv+DX_VAL, limit, botz, 0, ts);
			draw_vertex(xv+DX_VAL, limit, mesh_height[xy_ix][i  ], 0, ts);
			draw_vertex(xv,        limit, mesh_height[xy_ix][i-1], 0, ts);
		}
		xv += DX_VAL;
	}
	glNormal3f(1.0, 0.0, 0.0);

	for (int i = 1; i < MESH_Y_SIZE; ++i) { // x sides
		for (unsigned d = 0; d < 2; ++d) {
			int const xy_ix(d ? lx : 0);
			float const limit(d ? x2 : x1);
			draw_vertex(limit, yv,        botz, 1, ts);
			draw_vertex(limit, yv+DY_VAL, botz, 1, ts);
			draw_vertex(limit, yv+DY_VAL, mesh_height[i][xy_ix  ], 1, ts);
			draw_vertex(limit, yv,        mesh_height[i-1][xy_ix], 1, ts);
		}
		yv += DY_VAL;
	}
	glEnd();
	set_lighted_sides(1);
	glDisable(GL_TEXTURE_2D);
}


class water_renderer {

	int check_zvals;
	float tex_scale;
	colorRGBA color;

	void draw_vert(float x, float y, float z, bool in_y, bool neg_edge) const;
	void draw_x_sides(bool neg_edge) const;
	void draw_y_sides(bool neg_edge) const;
	void draw_sides(unsigned ix) const;

public:
	water_renderer(int ix, int iy, int cz) : check_zvals(cz), tex_scale(W_TEX_SCALE0/Z_SCENE_SIZE) {}
	void draw();
};


void water_renderer::draw_vert(float x, float y, float z, bool in_y, bool neg_edge) const { // in_y is slice orient

	colorRGBA c(color);
	point p(x, y, z), v(get_camera_pos());

	if ((v[!in_y] - p[!in_y] < 0.0) ^ neg_edge) { // camera viewing the inside face of the water side
		do_line_clip_scene(p, v, zbottom, z);
		float const atten(WATER_COL_ATTEN*p2p_dist(p, v));
		atten_by_water_depth(&c.R, atten);
		c.A = CLIP_TO_01(atten);
	}
	set_color(c);
	draw_vertex(x, y, z, in_y, tex_scale);
}


void water_renderer::draw_x_sides(bool neg_edge) const {

	int const end_val(neg_edge ? 0 : MESH_X_SIZE-1);
	float const limit(neg_edge ? -X_SCENE_SIZE : X_SCENE_SIZE-DX_VAL);
	float yv(-Y_SCENE_SIZE);
	bool in_quads(0);
	glNormal3f(-1.0, 0.0, 0.0);

	for (int i = 1; i < MESH_Y_SIZE; ++i) { // x sides
		float const mh1(mesh_height[i][end_val]), mh2(mesh_height[i-1][end_val]);
		float const wm1(water_matrix[i][end_val] - SMALL_NUMBER), wm2(water_matrix[i-1][end_val] - SMALL_NUMBER);

		if (!check_zvals || mh1 < wm1 || mh2 < wm2) {
			if (!in_quads) {glBegin(GL_QUADS); in_quads = 1;}
			draw_vert(limit, yv,        wm2,           1, neg_edge);
			draw_vert(limit, yv+DY_VAL, wm1,           1, neg_edge);
			draw_vert(limit, yv+DY_VAL, min(wm1, mh1), 1, neg_edge);
			draw_vert(limit, yv,        min(wm2, mh2), 1, neg_edge);
		}
		yv += DY_VAL;
	}
	if (in_quads) glEnd();
}


void water_renderer::draw_y_sides(bool neg_edge) const {

	int const end_val(neg_edge ? 0 : MESH_Y_SIZE-1);
	float const limit(neg_edge ? -Y_SCENE_SIZE : Y_SCENE_SIZE-DY_VAL);
	float xv(-X_SCENE_SIZE);
	bool in_quads(0);
	glNormal3f(0.0, 1.0, 0.0);
	
	for (int i = 1; i < MESH_X_SIZE; ++i) { // y sides
		float const mh1(mesh_height[end_val][i]), mh2(mesh_height[end_val][i-1]);
		float const wm1(water_matrix[end_val][i] - SMALL_NUMBER), wm2(water_matrix[end_val][i-1] - SMALL_NUMBER);

		if (!check_zvals || mh1 < wm1 || mh2 < wm2) {
			if (!in_quads) {glBegin(GL_QUADS); in_quads = 1;}
			draw_vert(xv,        limit, wm2,           0, neg_edge);
			draw_vert(xv+DX_VAL, limit, wm1,           0, neg_edge);
			draw_vert(xv+DX_VAL, limit, min(wm1, mh1), 0, neg_edge);
			draw_vert(xv,        limit, min(wm2, mh2), 0, neg_edge);
		}
		xv += DX_VAL;
	}
	if (in_quads) glEnd();
}


void water_renderer::draw_sides(unsigned ix) const {

	switch (ix) { // xn xp yn yp
		case 0: draw_x_sides(1); break;
		case 1: draw_x_sides(0); break;
		case 2: draw_y_sides(1); break;
		case 3: draw_y_sides(0); break;
		default: assert(0);
	}
}


void water_renderer::draw() { // modifies color

	select_water_ice_texture(color);
	set_color(color);
	set_fill_mode();
	set_lighted_sides(2);
	enable_blend();
	point const camera(get_camera_pos());
	float const pts[4][2] = {{-X_SCENE_SIZE, 0.0}, {X_SCENE_SIZE, 0.0}, {0.0, -Y_SCENE_SIZE}, {0.0, Y_SCENE_SIZE}};
	vector<pair<float, unsigned> > sides(4);

	for (unsigned i = 0; i < 4; ++i) {
		sides[i] = make_pair(-distance_to_camera_sq(point(pts[i][0], pts[i][1], water_plane_z)), i);
	}
	sort(sides.begin(), sides.end()); // largest to smallest distance

	for (unsigned i = 0; i < 4; ++i) {
		draw_sides(sides[i].second); // draw back to front
	}
	disable_blend();
	set_specular(0.0, 1.0);
	set_lighted_sides(1);
	glDisable(GL_TEXTURE_2D);
}


void draw_water_sides(int check_zvals) {

	water_renderer wr(resolution, resolution, check_zvals);
	wr.draw();
}


void setup_water_plane_texgen(float s_scale, float t_scale) {

	vector3d const wdir(vector3d(wind.x, wind.y, 0.0).get_norm());// wind.z is probably 0.0 anyway (nominal 1,0,0)
	float const tscale(W_TEX_SCALE0/Z_SCENE_SIZE), xscale(tscale*wdir.x), yscale(tscale*wdir.y);
	float const tdx(tscale*(xoff2 - xoff)*DX_VAL + water_xoff), tdy(tscale*(yoff2 - yoff)*DY_VAL + water_yoff);
	setup_texgen_full(s_scale*xscale, s_scale*yscale, 0.0, s_scale*(tdx*wdir.x + tdy*wdir.y), -t_scale*yscale, t_scale*xscale, 0.0, t_scale*(-tdx*wdir.y + tdy*wdir.x), GL_EYE_LINEAR);
}


void set_water_plane_uniforms(shader_t &s) {

	s.add_uniform_float("wave_time",      wave_time);
	s.add_uniform_float("wave_amplitude", min(1.0, 1.5*wind.mag())); // No waves if (temperature < W_FREEZE_POINT)?
	s.add_uniform_float("water_plane_z",  water_plane_z);
}


// texture units used: 0: reflection texture, 1: water normal map, 2: mesh height texture
void draw_water_plane(float zval, unsigned reflection_tid) {

	if (DISABLE_WATER) return;
	colorRGBA color;
	select_water_ice_texture(color, (combined_gu ? &univ_temp : &init_temperature), 1);
	bool const reflections(!(display_mode & 0x20));
	color.alpha *= 0.5;

	if (animate2 && temperature > W_FREEZE_POINT) {
		water_xoff -= WATER_WIND_EFF*wind.x*fticks;
		water_yoff -= WATER_WIND_EFF*wind.y*fticks;
		wave_time  += fticks;
	}
	if (light_factor >= 0.4 && get_sun_pos().z < water_plane_z) {set_specular(0.0, 1.0);} // has sun but it's below the water level
	point const camera(get_camera_pos());
	vector3d(0.0, 0.0, ((camera.z < zval) ? -1.0 : 1.0)).do_glNormal();
	setup_water_plane_texgen(1.0, 1.0);
	set_fill_mode();
	enable_blend();
	shader_t s;
	colorRGBA rcolor;
	set_active_texture(0);

	if (reflection_tid) {
		glBindTexture(GL_TEXTURE_2D, reflection_tid);
		rcolor = WHITE;
	}
	else {
		select_texture(WHITE_TEX, 0);
		glGetFloatv(GL_FOG_COLOR, (float *)&rcolor);
		//blend_color(rcolor, bkg_color, get_cloud_color(), 0.75, 1);
	}
	bool const add_waves((display_mode & 0x0100) != 0 && wind.mag() > TOLERANCE);
	bool const rain_mode(add_waves && is_rain_enabled());
	rcolor.alpha = 0.5*(0.5 + color.alpha);
	s.setup_enabled_lights();
	s.set_prefix("#define USE_QUADRATIC_FOG", 1); // FS
	s.set_bool_prefix("reflections", reflections, 1); // FS
	s.set_bool_prefix("add_waves", add_waves, 1); // FS
	s.set_bool_prefix("add_noise", rain_mode, 1); // FS
	s.set_vert_shader("texture_gen.part+water_plane");
	s.set_frag_shader("linear_fog.part+ads_lighting.part*+fresnel.part*+water_plane");
	s.begin_shader();
	s.setup_fog_scale();
	s.add_uniform_int  ("reflection_tex", 0);
	s.add_uniform_color("water_color",    color);
	s.add_uniform_color("reflect_color",  rcolor);
	s.add_uniform_float("ripple_scale",   10.0);
	s.add_uniform_float("ripple_mag",     2.0);
	s.add_uniform_int  ("height_tex",     2);

	// waves (as normal map)
	select_multitex(WATER_NORMAL_TEX, 1, 0);
	s.add_uniform_int("water_normal_tex", 1);
	set_water_plane_uniforms(s);

	if (rain_mode) {s.add_uniform_float("noise_time", frame_counter);} // rain ripples
	set_color(WHITE);
	draw_tiled_terrain_water(s, zval);
	s.end_shader();
	disable_blend();
	set_specular(0.0, 1.0);
	disable_textures_texgen();
	glEnable(GL_LIGHTING);
}


