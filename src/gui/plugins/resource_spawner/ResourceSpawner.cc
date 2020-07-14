/*
 * Copyright (C) 2020 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/
#include <ignition/msgs/boolean.pb.h>
#include <ignition/msgs/stringmsg.pb.h>

#include <sdf/Root.hh>
#include <sdf/parser.hh>

#include <ignition/common/Console.hh>
#include <ignition/common/Profiler.hh>
#include <ignition/common/Filesystem.hh>
#include <ignition/gui/Application.hh>
#include <ignition/gui/MainWindow.hh>
#include <ignition/plugin/Register.hh>
#include <ignition/transport/Node.hh>
#include <ignition/transport/Publisher.hh>

#include "ignition/gazebo/EntityComponentManager.hh"
#include "ignition/gazebo/gui/GuiEvents.hh"

#include "ResourceSpawner.hh"

namespace ignition::gazebo
{
  class ResourceSpawnerPrivate
  {
    /// \brief Ignition communication node.
    public: transport::Node node;

    /// \brief The grid model that the qml gridview reflects
    public: GridModel gridModel;

    public: PathModel pathModel;
  };
}

using namespace ignition;
using namespace gazebo;

/////////////////////////////////////////////////
PathModel::PathModel() : QStandardItemModel()
{
}

/////////////////////////////////////////////////
void PathModel::AddPath(const std::string &_path)
{
  IGN_PROFILE_THREAD_NAME("Qt thread");
  IGN_PROFILE("PathModel::AddPath");
  QStandardItem *parentItem{nullptr};

  parentItem = this->invisibleRootItem();

  auto localModel = new QStandardItem(QString::fromStdString(_path));
  localModel->setData(QString::fromStdString(_path),
                      this->roleNames().key("path"));

  ignwarn << "Adding " << _path << std::endl;
  parentItem->appendRow(localModel);
}

/////////////////////////////////////////////////
QHash<int, QByteArray> PathModel::roleNames() const
{
  return
  {
    std::pair(100, "path"),
  };
}

/////////////////////////////////////////////////
GridModel::GridModel() : QStandardItemModel()
{
}

/////////////////////////////////////////////////
void GridModel::AddLocalModel(LocalModel &_model)
{
  IGN_PROFILE_THREAD_NAME("Qt thread");
  IGN_PROFILE("GridModel::AddLocalModel");
  QStandardItem *parentItem{nullptr};

  parentItem = this->invisibleRootItem();

  auto localModel = new QStandardItem(QString::fromStdString(_model.name));
  localModel->setData(QString::fromStdString(_model.thumbnailPath),
                      this->roleNames().key("thumbnail"));
  localModel->setData(QString::fromStdString(_model.name),
                      this->roleNames().key("name"));
  localModel->setData(QString::fromStdString(_model.sdfPath),
                      this->roleNames().key("sdf"));

  parentItem->appendRow(localModel);
}

/////////////////////////////////////////////////
QHash<int, QByteArray> GridModel::roleNames() const
{
  return
  {
    std::pair(100, "thumbnail"),
    std::pair(101, "name"),
    std::pair(102, "sdf"),
  };
}

/////////////////////////////////////////////////
ResourceSpawner::ResourceSpawner()
  : ignition::gui::Plugin(),
  dataPtr(std::make_unique<ResourceSpawnerPrivate>())
{
  ignition::gui::App()->Engine()->rootContext()->setContextProperty(
      "LocalModelList", &this->dataPtr->gridModel);
  ignition::gui::App()->Engine()->rootContext()->setContextProperty(
      "PathList", &this->dataPtr->pathModel);
}

/////////////////////////////////////////////////
ResourceSpawner::~ResourceSpawner() = default;

/////////////////////////////////////////////////
void ResourceSpawner::LoadLocalModel(const std::string &_path)
{
  std::string fileName = common::basename(_path);
  if (!common::isFile(_path) || fileName != "model.config")
    return;

  // If we have found model.config, extract thumbnail and sdf
  LocalModel model;
  std::string modelPath = common::parentPath(_path);
  std::string thumbnailPath = common::joinPaths(modelPath, "thumbnails");
  std::string configFileName = common::joinPaths(modelPath, "model.config");
  tinyxml2::XMLDocument doc;
  doc.LoadFile(configFileName.c_str());
  auto modelXml = doc.FirstChildElement("model");

  if (modelXml)
  {
    auto modelName = modelXml->FirstChildElement("name");
    if (modelName)
      model.name = modelName->GetText();
  }
  std::string sdfPath = sdf::getModelFilePath(modelPath);
  model.sdfPath = sdfPath;

  // Get first thumbnail image found
  if (common::exists(thumbnailPath))
  {
    for (common::DirIter file(thumbnailPath);
         file != common::DirIter(); ++file)
    {
      std::string current(*file);
      if (common::isFile(current))
      {
        std::string thumbnailFileName = common::basename(current);
        std::string::size_type thumbnailExtensionIndex =
          thumbnailFileName.rfind(".");
        std::string thumbnailFileExtension =
          thumbnailFileName.substr(thumbnailExtensionIndex + 1);
        // The standard image types QML supports
        if (thumbnailFileExtension == "png"  ||
            thumbnailFileExtension == "jpg"  ||
            thumbnailFileExtension == "jpeg" ||
            thumbnailFileExtension == "svg")
        {
          model.thumbnailPath = current;
          break;
        }
      }
    }
  }
  this->dataPtr->gridModel.AddLocalModel(model);
}

/////////////////////////////////////////////////
void ResourceSpawner::FindLocalModels(const std::string &_path)
{
  std::string path = _path;
  if (common::isDirectory(path))
  {
    for (common::DirIter file(path); file != common::DirIter(); ++file)
    {
      std::string currentPath(*file);
      if (common::isDirectory(currentPath))
      {
        std::string modelConfigPath =
          common::joinPaths(currentPath, "model.config");
          this->LoadLocalModel(modelConfigPath);
      }
      else
      {
        this->LoadLocalModel(currentPath);
      }
    }
  }
  else
  {
    this->LoadLocalModel(path);
  }
}

void ResourceSpawner::AddPath(const std::string &_path)
{
  this->dataPtr->pathModel.AddPath(_path);
}

/////////////////////////////////////////////////
void ResourceSpawner::LoadConfig(const tinyxml2::XMLElement *)
{
  if (this->title.empty())
    this->title = "Resource Spawner";

  // For shapes requests
  ignition::gui::App()->findChild
    <ignition::gui::MainWindow *>()->installEventFilter(this);

  msgs::StringMsg_V res;
  bool result;
  bool executed = this->dataPtr->node.Request("/gazebo/resource_paths/get", 5000, res, result);
  if (!executed || !result || res.data_size() < 1)
  {
    ignwarn << "IGN_GAZEBO_RESOURCE_PATH not found."   \
               "Set this environment variable to the " \
               "path where your models are located.\n";
    return;
  }

  for (size_t i = 0; i < res.data_size(); i++)
  {
    std::string path = res.data(i);
    this->AddPath(res.data(i));
  }

  std::string path = res.data(0);
  this->FindLocalModels(path);
}

/////////////////////////////////////////////////
void ResourceSpawner::OnResourceSpawn(const QString &_sdfPath)
{
  std::string modelSdfPath = _sdfPath.toStdString();

  // Parse the sdf from the path
  std::ifstream nameFileout;
  nameFileout.open(modelSdfPath);
  std::string line;
  std::string modelSdfString = "";
  while (std::getline(nameFileout, line))
    modelSdfString += line + "\n";

  auto event = new gui::events::SpawnPreviewModel(modelSdfString);
  ignition::gui::App()->sendEvent(
      ignition::gui::App()->findChild<ignition::gui::MainWindow *>(),
      event);
}

// Register this plugin
IGNITION_ADD_PLUGIN(ignition::gazebo::ResourceSpawner,
                    ignition::gui::Plugin)
