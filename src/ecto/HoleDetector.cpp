#include <ecto_pcl/ecto_pcl.hpp>
#include <ecto_pcl/pcl_cell.hpp>

#include <sensor_msgs/PointCloud2.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl_ros/point_cloud.h>

#include <pcl/filters/extract_indices.h>
#include <pcl/filters/project_inliers.h>

#include <pcl/sample_consensus/sac_model_plane.h>

#include <pcl/surface/convex_hull.h>

#include <pcl/common/common.h>
#include <pcl/common/angles.h>
#include <pcl/common/transforms.h>

#include <pcl/filters/extract_indices.h>

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <Eigen/Dense>

#include <limits>

#include<transparent_object_reconstruction/Holes.h>
#include<transparent_object_reconstruction/tools.h>

struct HoleDetector
{
  static void calcPlaneTransformation (Eigen::Vector3f plane_normal,
      const Eigen::Vector3f &origin, Eigen::Affine3f &transformation)
  {
    Eigen::Vector3f ortho_to_normal = Eigen::Vector3f::Unit (3,1); // (0, 1, 0)

    float x, y, z;
    double angle = ::pcl::getAngle3D (Eigen::Vector4f (plane_normal[0],
          plane_normal[1], plane_normal[2], 0.0f),
        Eigen::Vector4f::Unit (4,2));
    if (fabs (angle - M_PI) < angle)
    {
      angle = fabs (angle - M_PI);
    }
    if (angle < ::pcl::deg2rad (.5f))
    {
      plane_normal.normalize ();
    }
    else
    {
      if (fabs (plane_normal[2]) > std::numeric_limits<float>::epsilon ())
      {
        x = y = 1.0f; // chosen arbitrarily
        z = plane_normal[0] + plane_normal[1];
        z /= -plane_normal[2];
      }
      else if (fabs (plane_normal[1]) > std::numeric_limits<float>::epsilon ())
      {
        x = z = 1.0f; // chosen arbitrarily
        y = plane_normal[0];
        y /= -plane_normal[1];
      }
      else
      {
        x = .0f;
        y = z = 1.0f; // chosen arbitrarily
      }
      ortho_to_normal = Eigen::Vector3f (x, y, z);
      ortho_to_normal.normalize ();
    }
    ::pcl::getTransformationFromTwoUnitVectorsAndOrigin (ortho_to_normal,
        plane_normal, origin, transformation);
  }

  template <typename PointT>
    void projectBorderAndCreateHull (const boost::shared_ptr<::pcl::PointCloud<PointT> > &border_cloud,
        boost::shared_ptr<::pcl::PointCloud<PointT> > &convex_hull, ::pcl::PointIndices &convex_hull_indices)
    {
      // TODO: which projection (perspective, closest dist) should be used here?
      auto proj_border_cloud = boost::make_shared<::pcl::PointCloud<PointT> > ();
      typename ::pcl::ProjectInliers<PointT> proj_border;
      proj_border.setInputCloud (border_cloud);
      proj_border.setModelType (pcl::SACMODEL_PLANE);
      proj_border.setModelCoefficients (*model_);
      proj_border.filter (*proj_border_cloud);

      // compute the convex hull of the (projected) border and retrieve the point indices
      typename ::pcl::ConvexHull<PointT> c_hull;
      c_hull.setInputCloud (proj_border_cloud);
      c_hull.setDimension (2);
      c_hull.reconstruct (*convex_hull);
      c_hull.getHullPointIndices (convex_hull_indices);
    }

  // TODO: more sophisticated test could check if the hull only touches the border (with on point)
  // or if there are consecutive points that touch the same border
  template <typename PointT>
    void get2DHullBBox (boost::shared_ptr<const ::pcl::PointCloud<PointT> > &cloud,
        const std::vector<Eigen::Vector2i> &convex_hull,
        Eigen::Vector2i &min_bbox, Eigen::Vector2i &max_bbox, bool &touches_border)
    {
      min_bbox = Eigen::Vector2i (cloud->width, cloud->height);
      max_bbox = Eigen::Vector2i::Zero ();

      std::vector<Eigen::Vector2i>::const_iterator hull_it = convex_hull.begin ();
      while (hull_it != convex_hull.end ())
      {
        if ((*hull_it)[0] < min_bbox[0])
          min_bbox[0] = (*hull_it)[0];
        if ((*hull_it)[1] < min_bbox[1])
          min_bbox[1] = (*hull_it)[1];
        if ((*hull_it)[0] > max_bbox[0])
          max_bbox[0] = (*hull_it)[0];
        if ((*hull_it)[1] > max_bbox[1])
          max_bbox[1] = (*hull_it)[1];
        hull_it++;
      }

      if (min_bbox[0] == 0 || min_bbox[1] == 0 ||
          max_bbox[0] == cloud->width - 1 || max_bbox[1] == cloud->height - 1)
      {
        touches_border = true;
      }
      else
      {
        touches_border = false;
      }
    }


  template <typename PointT>
    float holeBorderLowerBoundDist2 (boost::shared_ptr<const ::pcl::PointCloud<PointT> > &hole_border_a,
        boost::shared_ptr<const ::pcl::PointCloud<PointT> > &hole_border_b)
    {
      Eigen::Vector4f min_a, min_b, max_a, max_b;
      ::pcl::getMinMax3D<PointT> (*hole_border_a, min_a, max_a);
      ::pcl::getMinMax3D<PointT> (*hole_border_b, min_b, max_b);

      Eigen::Vector3f min_dist = Eigen::Vector3f::Zero ();
      // determine minimal distance of bboxes in each dimension
      for (size_t i = 0; i < 3; ++i)
      {
        if (max_b[i] < min_a[i])
        {
          min_dist[i] = min_a[i] - max_b[i];
        }
        else if (max_a[i] < min_b[i])
        {
          min_dist[i] = min_b[i] - max_a[i];
        }
      }

      return min_dist.dot (min_dist);
    }


  template <typename PointT>
    float minHoleBorderDist (boost::shared_ptr<const ::pcl::PointCloud<PointT> > &hole_border_a,
        boost::shared_ptr<const ::pcl::PointCloud<PointT> > &hole_border_b)
    {
      typename ::pcl::PointCloud<PointT>::VectorType::const_iterator p_it = hole_border_a->points.begin ();
      typename ::pcl::PointCloud<PointT>::VectorType::const_iterator line_end_it;
      PointT line_start;
      float min_dist = std::numeric_limits<float>::max ();
      float dist_to_line;

      while (p_it != hole_border_a->points.end ())
      {
        line_start = hole_border_b->points.back ();
        line_end_it = hole_border_b->points.begin ();
        while (line_end_it != hole_border_b->points.end ())
        {
          dist_to_line = lineToPointDistance (line_start, *line_end_it, *p_it);
          if (dist_to_line < min_dist)
          {
            min_dist = dist_to_line;
          }
          line_start = *line_end_it;
          line_end_it++;
        }
        p_it++;
      }
      return min_dist;
    }


  template <typename PointT>
    void addRemoveIndices (boost::shared_ptr<const ::pcl::PointCloud<PointT> > &cloud,
        const std::vector<Eigen::Vector2i> &convex_hull,
        ::pcl::PointIndices::Ptr &remove_indices, bool &touches_border)
    {
      // retrieve 2D bbox of convex hull
      Eigen::Vector2i min, max;
      get2DHullBBox (cloud, convex_hull, min, max, touches_border);

      // reserve the upper limit of newly needed entries into remove_indices
      remove_indices->indices.reserve (remove_indices->indices.size () + (max[0] - min[0]) * (max[1] - min[1]));
      Eigen::Vector2i query_point;
      int point_index;

      for (int u = min[0]; u < max[0]; ++u)
      {
        query_point[0] = u;
        for (int v = min[1]; v < max[1]; ++v)
        {
          query_point[1] = v;
          // check if point is inside the convex hull
          if (pointInPolygon2D (convex_hull, query_point))
          {
            if (::pcl::isFinite (cloud->at (u, v)))
            {
              convert2DCoordsToIndex (cloud, u, v, point_index);
              remove_indices->indices.push_back (point_index);
            }
          }
        }
      }
    }

  static bool pointInPolygon2D (const std::vector<Eigen::Vector2i> &polygon,
      const Eigen::Vector2i &query_point)
  {
    bool inside = false;

    std::vector<Eigen::Vector2i>::const_iterator start_it, end_it;
    start_it = polygon.end () - 1;  // last vertex
    end_it = polygon.begin ();
    bool start_above, end_above;

    start_above = (*start_it)[1] >= query_point[1] ? true: false;
    while (end_it != polygon.end ())
    {
      end_above = (*end_it)[1] >= query_point[1] ? true : false;

      if (start_above != end_above)
      {
        if (((*end_it)[1] - query_point[1]) * ((*end_it)[0] - (*start_it)[0]) <=
            ((*end_it)[1] - (*start_it)[1]) * ((*end_it)[0] - query_point[0]))
        {
          if (end_above)
          {
            inside = !inside;
          }
        }
        else
        {
          if (!end_above)
          {
            inside = !inside;
          }
        }
      }
      start_above = end_above;
      start_it = end_it;
      end_it++;
    }

    return inside;
  }

  template <typename PointT>
    static void recursiveNANGrowing (
        boost::shared_ptr<const ::pcl::PointCloud<PointT> > &cloud,
        int column, int row,
        std::vector<Eigen::Vector2i> &hole_2Dcoords,
        std::vector<Eigen::Vector2i> &border_2Dcoords,
        std::vector<std::vector<bool> > &visited)
    {
      // check dimension
      if (column < 0 || row < 0 ||
          column >= cloud->width || row >= cloud->height)
        return;
      // check if already visited and nan-point -- borders may be part of two neighboring regions
      auto p = cloud->at (column, row);
      if (visited[column][row] && !pcl_isfinite (p.x))
        return;
      visited[column][row] = true;
      Eigen::Vector2i coordinates (column, row);
      if (pcl_isfinite (p.x))
      {
        border_2Dcoords.push_back (coordinates);
        return;
      }
      hole_2Dcoords.push_back (coordinates);
      recursiveNANGrowing (cloud, column - 1, row, hole_2Dcoords, border_2Dcoords, visited);
      recursiveNANGrowing (cloud, column + 1, row, hole_2Dcoords, border_2Dcoords, visited);
      recursiveNANGrowing (cloud, column, row - 1, hole_2Dcoords, border_2Dcoords, visited);
      recursiveNANGrowing (cloud, column, row + 1, hole_2Dcoords, border_2Dcoords, visited);
    }

  template <typename PointT>
    static bool convertIndexTo2DCoords (
        boost::shared_ptr<const ::pcl::PointCloud<PointT> > &cloud,
        int index, int &x_coord, int &y_coord)
    {
      if (!cloud->isOrganized ())
      {
        x_coord = y_coord = -1;
        std::cerr << "convertIndexTo2DCoords: cloud needs to be organized" << std::endl;
        return false;
      }
      if (index < 0 || index >= cloud->points.size ())
      {
        x_coord = y_coord = -1;
        std::cerr << "convertIndexTo2DCoords: specified point index " << index << " invalid" << std::endl;
        return false;
      }
      y_coord = index / cloud->width;
      x_coord = index - cloud->width * y_coord;

      return true;
    }

  template <typename PointT>
    static bool convert2DCoordsToIndex (
        boost::shared_ptr<const ::pcl::PointCloud<PointT> > &cloud,
        int x_coord, int y_coord, int &index)
    {
      if (x_coord < 0 || y_coord < 0)
      {
        std::cerr << "Image coordinates need to be larger than 0" << std::endl;
        return false;
      }
      if (!cloud->isOrganized () && y_coord > 1)
      {
        std::cerr << "Cloud not organized, but y_coord > 1" << std::endl;
        return false;
      }
      if (x_coord >= cloud->width || y_coord >= cloud->height)
      {
        std::cerr << "2D coordinates (" << x_coord << ", " << y_coord << ") outside"
          << " of cloud dimension " << cloud->width << "x" << cloud->height << std::endl;
        return false;
      }
      index = y_coord * cloud->width + x_coord;

      return true;
    }

  template <typename PointT>
    static void getBoundingBox2DConvexHull (
        boost::shared_ptr<const ::pcl::PointCloud<PointT> > &cloud,
        const ::pcl::PointIndices &hull_indices,
        Eigen::Vector2i &min, Eigen::Vector2i &max,
        std::vector<Eigen::Vector2i> &hull_2Dcoords)
    {
      // initialize output
      max[0] = max[1] = 0;
      min[0] = cloud->width;
      min[1] = cloud->height;

      hull_2Dcoords.clear ();
      hull_2Dcoords.reserve (hull_indices.indices.size ());

      int u, v;

      std::vector<int>::const_iterator index_it = hull_indices.indices.begin ();
      while (index_it != hull_indices.indices.end ())
      {
        if (convertIndexTo2DCoords (cloud, *index_it++, u, v))
        {
          hull_2Dcoords.push_back (Eigen::Vector2i (u, v));
          if (u < min[0])
            min[0] = u;
          if (v < min[1])
            min[1] = v;
          if (u > max[0])
            max[0] = u;
          if (v > max[1])
            max[1] = v;
        }
      }
    }

  static void declare_params (tendrils& params)
  {
    params.declare<size_t> ("min_hole_size", "Minimal numbers of connected pixels in the depth image to form a hole", 15);
    params.declare<float> ("inside_out_factor", "Determines if a nan-region is outside of table hull (#outside > #points_inside * inside_out_factor)", 2.0f);
    params.declare<float> ("plane_dist_threshold", "Distance threshold for plane classification", .02f);
    params.declare<float> ("min_distance_to_convex_hull", "Minimal distance to convex hull for overlapping holes", .05f);
  }

  static void declare_io ( const tendrils& params, tendrils& inputs, tendrils& outputs)
  {
    inputs.declare<::pcl::PointIndices::ConstPtr> ("hull_indices", "The indices describing the convex hull to the table surface.");
    inputs.declare<::pcl::ModelCoefficients::ConstPtr> ("model", "Model coefficients for the planar table surface.");
    outputs.declare<ecto::pcl::PointCloud> ("output", "Filtered Cloud.");
    outputs.declare<transparent_object_reconstruction::Holes::ConstPtr> ("holes", "Detected holes inside the table convex hull.");
    outputs.declare<::pcl::PointIndices::ConstPtr> ("remove_indices", "Indices of points inside the detected holes.");
  }

  void configure( const tendrils& params, const tendrils& inputs, const tendrils& outputs)
  {
    min_hole_size_ = params["min_hole_size"];
    inside_out_factor_ = params["inside_out_factor"];
    plane_dist_threshold_ = params["plane_dist_threshold"];
    min_distance_to_convex_hull_ = params["min_distance_to_convex_hull"];
    hull_indices_ = inputs["hull_indices"];
    model_ = inputs["model"];
    output_ = outputs["output"];
    holes_mgs_ = outputs["holes"];
    remove_indices_ = outputs["remove_indices"];
  }

  template <typename PointT>
    int process( const tendrils& inputs, const tendrils& outputs,
        boost::shared_ptr<const ::pcl::PointCloud<PointT> >& input)
    {
      Eigen::Vector3f plane_normal = Eigen::Vector3f ((*model_)->values[0],
          (*model_)->values[1], (*model_)->values[2]);

      Eigen::Vector2i table_min, table_max;
      std::vector<Eigen::Vector2i> hull_2Dcoords;
      std::vector<std::vector<Eigen::Vector2i> > all_hole_2Dcoords;
      std::vector<std::vector<Eigen::Vector2i> > all_border_2Dcoords;
      getBoundingBox2DConvexHull (input, **hull_indices_, table_min, table_max, hull_2Dcoords);

      // retrieve the 3D coordinates of the convex hull of the tabletop
      auto table_convex_hull = boost::make_shared<::pcl::PointCloud<PointT> > ();
      table_convex_hull->points.reserve ((*hull_indices_)->indices.size ());
      std::vector<int>::const_iterator hull_index_it = (*hull_indices_)->indices.begin ();
      while (hull_index_it != (*hull_indices_)->indices.end ())
      {
        table_convex_hull->points.push_back (input->points[*hull_index_it++]);
      }
      table_convex_hull->width = table_convex_hull->points.size ();
      table_convex_hull->height = 1;

      std::vector<std::vector<bool> > visited (input->width, std::vector<bool> (input->height, false));

      for (int u = table_min[0]; u < table_max[0]; ++u)
      {
        for (int v = table_min[1]; v < table_max[1]; ++v)
        {
          if (!visited[u][v] &&                                             // already used?
              pointInPolygon2D (hull_2Dcoords, Eigen::Vector2i (u, v)) &&   // inside hull?
              !(pcl::isFinite (input->at (u,v))))                           // nan point?
          {
            std::vector<Eigen::Vector2i> hole_2Dcoords;
            std::vector<Eigen::Vector2i> border_2Dcoords;
            recursiveNANGrowing (input, u, v, hole_2Dcoords, border_2Dcoords, visited);
            if (hole_2Dcoords.size () > *min_hole_size_)
            {
              all_hole_2Dcoords.push_back (hole_2Dcoords);
              all_border_2Dcoords.push_back (border_2Dcoords);
            }
          }
        }
      }

      // collected all nan-regions that contain at least 1 nan-pixel inside the convex hull of the table
      std::vector<std::vector<Eigen::Vector2i> >::const_iterator all_holes_it;
      std::vector<std::vector<Eigen::Vector2i> >::const_iterator all_borders_it;
      std::vector<Eigen::Vector2i>::const_iterator hole_it;
      all_holes_it = all_hole_2Dcoords.begin ();
      all_borders_it = all_border_2Dcoords.begin ();

      std::vector<std::vector<Eigen::Vector2i> > outside_holes;
      std::vector<std::vector<Eigen::Vector2i> > outside_borders;
      std::vector<std::vector<Eigen::Vector2i> > overlap_holes;
      std::vector<std::vector<Eigen::Vector2i> > overlap_borders;
      std::vector<std::vector<Eigen::Vector2i> > inside_holes;
      std::vector<std::vector<Eigen::Vector2i> > inside_borders;

      outside_holes.reserve (all_hole_2Dcoords.size ());
      outside_borders.reserve (all_hole_2Dcoords.size ());
      overlap_holes.reserve (all_hole_2Dcoords.size ());
      overlap_borders.reserve (all_hole_2Dcoords.size ());
      inside_holes.reserve (all_hole_2Dcoords.size ());
      inside_borders.reserve (all_hole_2Dcoords.size ());

      size_t inside, outside;

      while (all_holes_it != all_hole_2Dcoords.end ())
      {
        inside = outside = 0;
        hole_it = all_holes_it->begin ();
        while (hole_it != all_holes_it->end ())
        {
          if (pointInPolygon2D (hull_2Dcoords, *hole_it))
          {
            inside++;
          }
          else
          {
            outside++;
          }
          hole_it++;
        }
        // TODO: probably some fraction dependent on min_hole_size_ should be used
        if (outside > 0)
        {
          if (outside > inside * (*inside_out_factor_))
          {
            // TODO: could we use these later somewhere or can they be safely discarded?
            outside_holes.push_back (*all_holes_it);
            outside_borders.push_back (*all_borders_it);
          }
          else
          {
            overlap_holes.push_back (*all_holes_it);
            overlap_borders.push_back (*all_borders_it);
          }
        }
        else
        {
          inside_holes.push_back (*all_holes_it);
          inside_borders.push_back (*all_borders_it);
        }
        all_holes_it++;
        all_borders_it++;
      }
      // create array to store all non nan-points that are enclosed by the hole
      // and should be removed before clustering
      ::pcl::PointIndices::Ptr remove_indices (new ::pcl::PointIndices);
      std::vector<Eigen::Vector2i>::const_iterator coord_it;

      Eigen::Vector4f plane_coefficients ((*model_)->values[0], (*model_)->values[1],
          (*model_)->values[2], (*model_)->values[3]);

      // create representations for the holes completely inside convex hull of table top
      auto holes_msg= boost::make_shared<transparent_object_reconstruction::Holes>();
      holes_msg->convex_hulls.reserve (inside_holes.size ());
      for (size_t i = 0; i < inside_holes.size (); ++i)
      {
        auto border_cloud = boost::make_shared<::pcl::PointCloud<PointT> > ();
        border_cloud->header = input->header;
        border_cloud->points.reserve (inside_borders[i].size ());
        coord_it = inside_borders[i].begin ();
        while (coord_it != inside_borders[i].end ())
        {
          border_cloud->points.push_back (input->at ((*coord_it)[0], (*coord_it)[1]));
          coord_it++;
        }
        // set dimensions of border cloud
        border_cloud->width = border_cloud->points.size ();
        border_cloud->height = 1;

        double dist_sum = .0f;
        auto border_it = border_cloud->points.begin ();

        // check if hole border aligns with tabletop (or hole was caused by noise of tabletop obj)
        while (border_it != border_cloud->points.end ())
        {
          dist_sum += ::pcl::pointToPlaneDistance (*border_it++, plane_coefficients);
        }

        double avg_dist = dist_sum / static_cast<double> (border_cloud->points.size ());
        if (avg_dist > *plane_dist_threshold_)
        {
          // skip hole! should something more be done?
          // TODO: remove measurement points inside hole anyway?
          continue;
        }

        // project border into plane and retrieve convex hull
        auto conv_border_cloud = boost::make_shared<::pcl::PointCloud<PointT> > ();
        ::pcl::PointIndices conv_border_indices;
        projectBorderAndCreateHull (border_cloud, conv_border_cloud, conv_border_indices);

        std::vector<Eigen::Vector2i> convex_hull_polygon;
        convex_hull_polygon.reserve (conv_border_indices.indices.size ());
        std::vector<int>::const_iterator hull_it = conv_border_indices.indices.begin ();
        while (hull_it != conv_border_indices.indices.end ())
        {
          convex_hull_polygon.push_back (inside_borders[i][*hull_it++]);
        }
        // store indices of points that are inside the convex hull - these will be removed later
        bool touches_border;
        addRemoveIndices (input, convex_hull_polygon, remove_indices, touches_border);

        if (!touches_border)
        {
          // add current hole to Hole message
          sensor_msgs::PointCloud2 pc2;
          pcl::toROSMsg (*conv_border_cloud, pc2);
          holes_msg->convex_hulls.push_back (pc2);
        }
        // else discard the hole
      }

      // work on the holes that are partially inside the convex hull
      for (size_t i = 0; i < overlap_borders.size (); ++i)
      {
        unsigned int inside_points = 0;
        double dist_sum = 0.0f;
        // create temporary cloud for all border points inside the convex hull, so that we can check for hole artifacts later

        // create a marker to check which of the border points are inside the convex hull
        std::vector<bool> point_inside (overlap_borders[i].size (), false);

        coord_it = overlap_borders[i].begin ();
        // check for alignment of the points that are considered in the convex hull
        for (size_t j = 0; j < overlap_borders[i].size (); ++j)
        {
          if (pointInPolygon2D (hull_2Dcoords, overlap_borders[i][j]))
          {
            dist_sum += ::pcl::pointToPlaneDistance (input->at (overlap_borders[i][j][0],
                  overlap_borders[i][j][1]), plane_coefficients);
            inside_points++;
            point_inside[j] = true;
          }
        }

        double avg_dist = dist_sum / static_cast<double> (inside_points);
        if (avg_dist > *plane_dist_threshold_ * 3.0f) // TODO: perhaps remove the points with the largest 3 distances instead?
        {
          //skip hole! should something more be done?
          continue;
        }
        auto border_cloud = boost::make_shared<::pcl::PointCloud<PointT> > ();
        border_cloud->header = input->header;
        border_cloud->points.reserve (overlap_borders[i].size ());
        coord_it = overlap_borders[i].begin ();
        PointT border_p, projection;

        auto inside_hull_border = boost::make_shared<::pcl::PointCloud<PointT> > ();
        inside_hull_border->points.reserve (overlap_borders[i].size ());
        std::vector<bool>::const_iterator is_inside_it = point_inside.begin ();

        while (coord_it != overlap_borders[i].end ())
        {
          border_p = input->at ((*coord_it)[0], (*coord_it)[1]);
          if (::pcl::pointToPlaneDistance (border_p, plane_coefficients) > *plane_dist_threshold_ * 2.0f)
          {
            // compute perspective projection of border point onto plane
            if (projectPointOnPlane<PointT> (border_p, projection, plane_coefficients))
            {
              // if a projection exists, add it to the border cloud
              border_cloud->points.push_back (projection);
            }
            // otherwise discard this point
          }
          else
          {
            border_cloud->points.push_back (border_p);
          }
          // store all point (raw or projected) that are inside the convex hull of the table
          if (*is_inside_it++)
          {
            inside_hull_border->points.push_back (border_cloud->points.back ());
          }
          coord_it++;
        }
        inside_hull_border->width = inside_hull_border->points.size ();
        inside_hull_border->height = 1;

        if (border_cloud->points.size () > 0)
        {
          // determine if this hole is an 'artifact' near the edges of the table...
          typename ::pcl::PointCloud<PointT>::VectorType::const_iterator p_it = inside_hull_border->points.begin ();
          typename ::pcl::PointCloud<PointT>::VectorType::const_iterator table_hull_it;
          float max_min_dist = -std::numeric_limits<float>::max ();
          PointT line_point;
          float dist_to_line;
          // determine for each point of the hole border inside the hull the closest line of the convex hull
          while (p_it != inside_hull_border->points.end ())
          {
            float min_distance = std::numeric_limits<float>::max ();
            line_point = table_convex_hull->points.back ();
            table_hull_it = table_convex_hull->points.begin ();
            while (table_hull_it != table_convex_hull->points.end ())
            {
              dist_to_line = lineToPointDistance<PointT> (line_point, *table_hull_it, *p_it);
              if (dist_to_line < min_distance)
              {
                min_distance = dist_to_line;
              }
              line_point = *table_hull_it;
              table_hull_it++;
            }
            if (min_distance > max_min_dist)
            {
              max_min_dist = min_distance;
            }
            p_it++;
          }

          // the farthest point from the hole should be at least as far away as given threshold
          if (max_min_dist > *min_distance_to_convex_hull_)
          {
            // set dimensions of border cloud
            border_cloud->width = border_cloud->points.size ();
            border_cloud->height = 1;

            // project border into plane and retrieve convex hull
            auto conv_border_cloud = boost::make_shared<::pcl::PointCloud<PointT> > ();
            ::pcl::PointIndices conv_border_indices;
            projectBorderAndCreateHull (border_cloud, conv_border_cloud, conv_border_indices);

            std::vector<Eigen::Vector2i> convex_hull_polygon;
            convex_hull_polygon.reserve (conv_border_indices.indices.size ());
            std::vector<int>::const_iterator hull_it = conv_border_indices.indices.begin ();
            while (hull_it != conv_border_indices.indices.end ())
            {
              convex_hull_polygon.push_back (overlap_borders[i][*hull_it++]);
            }
            // store indices of points that are inside the convex hull - these will be removed later
            bool touches_border;
            addRemoveIndices (input, convex_hull_polygon, remove_indices, touches_border);

            if (!touches_border)
            {
              // add current hole to Hole message
              sensor_msgs::PointCloud2 pc2;
              pcl::toROSMsg (*conv_border_cloud, pc2);
              holes_msg->convex_hulls.push_back (pc2);
            }
            // else discard the hole
          }
        }
      }

      if (remove_indices->indices.size () > 1)
      {
        // depending on what we want to do later it might be more convenient, if the remove indices are ordered
        ::std::sort (remove_indices->indices.begin (), remove_indices->indices.end ());
        // copy indices into output (and make sure that no index is inserted more than once
        ::pcl::PointIndices::Ptr tmp_indices (new ::pcl::PointIndices);
        tmp_indices->indices.reserve (remove_indices->indices.size ());
        std::vector<int>::const_iterator index_it = remove_indices->indices.begin ();
        tmp_indices->indices.push_back (*index_it++);
        while (index_it != remove_indices->indices.end ())
        {
          if (*index_it != tmp_indices->indices.back ())
          {
            tmp_indices->indices.push_back (*index_it);
          }
          index_it++;
        }
        *remove_indices_ = tmp_indices;
      }
      else
      {
        *remove_indices_ = remove_indices;
      }

      // TODO: do we still need the filtered point cloud as output?
      // set all points in the point cloud to nan, if their index was contained in remove_indices
      typename ::pcl::ExtractIndices<PointT> extractor;
      auto filtered_cloud = boost::make_shared<::pcl::PointCloud<PointT> > ();
      extractor.setKeepOrganized (true);
      extractor.setNegative (true);
      extractor.setInputCloud (input);
      extractor.setIndices (remove_indices);
      extractor.filter (*filtered_cloud);

      *output_ = ecto::pcl::xyz_cloud_variant_t (filtered_cloud);
      *holes_mgs_ = holes_msg;

      return ecto::OK;
    }

  ecto::spore<size_t> min_hole_size_;
  ecto::spore<float> inside_out_factor_;
  ecto::spore<float> plane_dist_threshold_;
  ecto::spore<float> min_distance_to_convex_hull_;
  ecto::spore<::pcl::PointIndices::ConstPtr> hull_indices_;
  ecto::spore<::pcl::ModelCoefficients::ConstPtr> model_; //TODO: is this what I get from the segmentation?
  ecto::spore<ecto::pcl::PointCloud> output_;
  ecto::spore<transparent_object_reconstruction::Holes::ConstPtr> holes_mgs_;
  ecto::spore<::pcl::PointIndices::ConstPtr> remove_indices_;
};

ECTO_CELL(hole_detection, ecto::pcl::PclCell<HoleDetector>,
    "HoleDetector", "Extract a new cloud given an existing cloud and a set of indices to extract.");

