/*
 * Software License Agreement (Apache License)
 *
 * Copyright (c) 2014, Southwest Research Institute
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/*
 * process_path_generator.h
 *
 *  Created on: May 9, 2014
 *      Author: ros
 */

#ifndef PROCESS_PATH_GENERATOR_H_
#define PROCESS_PATH_GENERATOR_H_

#include "godel_process_path_generation/polygon_pts.hpp"

namespace godel_process_path
{

class ProcessPathGenerator
{
public:
  ProcessPathGenerator() {};
  virtual ~ProcessPathGenerator() {};

  bool configure(PolygonBoundaryCollection boundaries);
};

} /* namespace godel_process_path */
#endif /* PROCESS_PATH_GENERATOR_H_ */
