/* Copyright (c) 2016-2016, Vasil Dimov

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE. */

#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <ipfs/client.h>
#include <ipfs/http/transport-curl.h>
#include <ipfs/http/transport.h>

namespace ipfs {

Client::Client(const std::string& host, long port)
    : url_prefix_("http://" + host + ":" + std::to_string(port) + "/api/v0") {
  http_ = new http::TransportCurl();
}

Client::~Client() { delete http_; }

void Client::Id(Json* id) { FetchAndParseJson(MakeUrl("id"), id); }

void Client::Version(Json* version) {
  FetchAndParseJson(MakeUrl("version"), version);
}

void Client::ConfigGet(const std::string& key, Json* config) {
  std::string url;

  if (key.empty()) {
    url = MakeUrl("config/show");
  } else {
    url = MakeUrl("config", {{"arg", key}});
  }

  FetchAndParseJson(url, config);

  if (!key.empty()) {
    static const char* value = "Value";
    if (config->find(value) == config->end()) {
      throw std::runtime_error(
          std::string("Unexpected reply: valid JSON, but without the \"") +
          value + "\" property");
    }
    /* Convert
    {
      "Key": "Datastore",
      "Value": { "BloomFilterSize": 0, "GCPeriod": "1h", ... }
    }

    to

    { "BloomFilterSize": 0, "GCPeriod": "1h", ... }
    */
    *config = (*config)[value];
  }
}

void Client::ConfigSet(const std::string& key, const Json& value) {
  Json unused;
  FetchAndParseJson(MakeUrl("config", {{"arg", key}, {"arg", value.dump()}}),
                    &unused);
}

void Client::ConfigReplace(const Json& config) {
  std::stringstream unused;
  http_->Fetch(MakeUrl("config/replace"),
               {{"new_config.json", http::FileUpload::Type::kFileContents,
                 config.dump()}},
               &unused);
}

void Client::BlockGet(const std::string& block_id, std::iostream* block) {
  http_->Fetch(MakeUrl("block/get", {{"arg", block_id}}), {}, block);
}

void Client::BlockPut(const http::FileUpload& block, Json* stat) {
  FetchAndParseJson(MakeUrl("block/put"), {block}, stat);
}

void Client::BlockStat(const std::string& block_id, Json* stat) {
  FetchAndParseJson(MakeUrl("block/stat", {{"arg", block_id}}), stat);
}

void Client::FilesGet(const std::string& path, std::iostream* response) {
  http_->Fetch(MakeUrl("cat", {{"arg", path}}), {}, response);
}

void Client::FilesAdd(const std::vector<http::FileUpload>& files,
                      Json* result) {
  std::stringstream body;

  http_->Fetch(MakeUrl("add", {{"progress", "true"}}), files, &body);

  /* The reply consists of multiple lines, each one of which is a JSON, for
  example:

  {"Name":"foo.txt","Bytes":4}
  {"Name":"foo.txt","Hash":"QmWPyMW2u7J2Zyzut7TcBMT8pG6F2cB4hmZk1vBJFBt1nP"}
  {"Name":"bar.txt","Bytes":1176}
  {"Name":"bar.txt","Hash":"QmVjQsMgtRsRKpNM8amTCDRuUPriY8tGswsTpo137jPWwL"}

  we convert that into a single JSON like:

  [
    { "path": "foo.txt", "hash": "QmWP...", "size": 4 },
    { "path": "bar.txt", "hash": "QmVj...", "size": 1176 }
  ]

  and return it to the caller. */

  /* A temporary JSON object to facilitate creating the result in case the
  reply lines are out of order. This one looks like:
  {
    "foo.txt": { "path": "foo.txt", "hash": "QmWP...", "size": 4 }
    "bar.txt": { "path": "foo.txt", "hash": "QmVj...", "size": 1176 }
  }
  */
  Json temp;

  std::string line;
  for (size_t i = 1; std::getline(body, line); ++i) {
    Json json_chunk;

    ParseJson(line, &json_chunk);

    static const char* name = "Name";

    if (json_chunk.find(name) == json_chunk.end()) {
      throw std::runtime_error(
          std::string("Unexpected reply: valid JSON, but without the \"") +
          name + "\" property on line " + std::to_string(i) + ":\n" +
          body.str());
    }

    const std::string& path = json_chunk[name];
    temp[path]["path"] = path;

    static const char* hash = "Hash";
    if (json_chunk.find(hash) != json_chunk.end()) {
      temp[path]["hash"] = json_chunk[hash];
    }

    static const char* bytes = "Bytes";
    if (json_chunk.find(bytes) != json_chunk.end()) {
      temp[path]["size"] = json_chunk[bytes];
    }
  }

  for (Json::iterator it = temp.begin(); it != temp.end(); ++it) {
    result->push_back(it.value());
  }
}

void Client::ObjectNew(std::string* object_id) {
  Json response;

  FetchAndParseJson(MakeUrl("object/new"), &response);

  static const char* hash = "Hash";

  if (response.find(hash) == response.end()) {
    throw std::runtime_error(
        std::string("Unexpected reply: valid JSON, but without the \"") + hash +
        "\" property:\n" + response.dump());
  }

  *object_id = response[hash];
}

void Client::ObjectPut(const Json& object, Json* object_stored) {
  FetchAndParseJson(
      MakeUrl("object/put", {{"inputenc", "json"}}),
      {{"node.json", http::FileUpload::Type::kFileContents, object.dump()}},
      object_stored);
}

void Client::ObjectGet(const std::string& object_id, Json* object) {
  FetchAndParseJson(MakeUrl("object/get", {{"arg", object_id}}), object);
}

void Client::ObjectData(const std::string& object_id, std::string* data) {
  std::stringstream body;

  http_->Fetch(MakeUrl("object/data", {{"arg", object_id}}), {}, &body);

  *data = body.str();
}

void Client::ObjectStat(const std::string& object_id, Json* stat) {
  FetchAndParseJson(MakeUrl("object/stat", {{"arg", object_id}}), stat);
}

void Client::FetchAndParseJson(const std::string& url, Json* response) {
  FetchAndParseJson(url, {}, response);
}

void Client::FetchAndParseJson(const std::string& url,
                               const std::vector<http::FileUpload>& files,
                               Json* response) {
  std::stringstream body;

  http_->Fetch(url, files, &body);

  ParseJson(body.str(), response);
}

void Client::ParseJson(const std::string& input, Json* result) {
  try {
    *result = Json::parse(input);
  } catch (const std::exception& e) {
    throw std::runtime_error(std::string(e.what()) + "\nInput JSON:\n" + input);
  }
}

std::string Client::MakeUrl(
    const std::string& path,
    const std::vector<std::pair<std::string, std::string>>& parameters) {
  std::string url = url_prefix_ + "/" + path +
                    "?stream-channels=true&json=true&encoding=json";

  for (auto& parameter : parameters) {
    std::string name_url_encoded;
    http_->UrlEncode(parameter.first, &name_url_encoded);

    std::string value_url_encoded;
    http_->UrlEncode(parameter.second, &value_url_encoded);

    url += "&" + name_url_encoded + "=" + value_url_encoded;
  }

  return url;
}
} /* namespace ipfs */
