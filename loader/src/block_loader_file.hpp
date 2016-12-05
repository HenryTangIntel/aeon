/*
 Copyright 2016 Nervana Systems Inc.
 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.
*/

#pragma once

#include <future>

#include "manifest_csv.hpp"
#include "buffer_in.hpp"
#include "block_loader.hpp"
#include "util.hpp"

/* block_loader_file
 *
 * Loads blocks of files from a Manifest into a BufferPair.
 *
 */

namespace nervana
{
    class block_loader_file;
}

class nervana::block_loader_file : public block_loader
{
public:
    typedef std::vector<std::pair<std::vector<char>, std::exception_ptr>> buffer_t;
    block_loader_file(std::shared_ptr<nervana::manifest_csv> manifest, float subset_fraction, uint32_t block_size);

    void load_block(nervana::buffer_in_array& dest, uint32_t block_num) override;
    void load_file(std::vector<char>& buff, const std::string& filename);
    uint32_t object_count() override;

private:
    void generate_subset(const std::shared_ptr<nervana::manifest_csv>& manifest, float subset_fraction);
    buffer_t fetch_block(uint32_t block_num);
    void prefetch_block(uint32_t block_num) override;

    const std::shared_ptr<nervana::manifest_csv> m_manifest;
    std::future<buffer_t>     m_future;
    size_t   m_elements_per_record;
};
