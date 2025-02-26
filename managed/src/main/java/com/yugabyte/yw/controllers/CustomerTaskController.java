// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.controllers;

import com.typesafe.config.Config;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.google.inject.Inject;
import com.yugabyte.yw.commissioner.Commissioner;
import com.yugabyte.yw.common.ApiResponse;
import com.yugabyte.yw.forms.CustomerTaskFormData;
import com.yugabyte.yw.forms.SubTaskFormData;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.forms.UniverseResp;
import com.yugabyte.yw.models.*;
import com.yugabyte.yw.models.helpers.TaskType;
import io.ebean.Query;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import play.libs.Json;
import play.mvc.Result;

import java.util.*;

public class CustomerTaskController extends AuthenticatedController {

  @Inject
  Commissioner commissioner;

  protected static final int TASK_HISTORY_LIMIT = 6;
  public static final Logger LOG = LoggerFactory.getLogger(CustomerTaskController.class);

  private List<SubTaskFormData> fetchFailedSubTasks(UUID parentUUID) {
    Query<TaskInfo> subTaskQuery = TaskInfo.find.query().where()
      .eq("parent_uuid", parentUUID)
      .eq("task_state", TaskInfo.State.Failure.name())
      .orderBy("position desc");
    Set<TaskInfo> result = subTaskQuery.findSet();
    List<SubTaskFormData> subTasks = new ArrayList<>();
    for (TaskInfo taskInfo : result) {
      SubTaskFormData subTaskData = new SubTaskFormData();
      subTaskData.subTaskUUID = taskInfo.getTaskUUID();
      subTaskData.subTaskType = taskInfo.getTaskType().name();
      subTaskData.subTaskState = taskInfo.getTaskState().name();
      subTaskData.creationTime = taskInfo.getCreationTime();
      subTaskData.subTaskGroupType = taskInfo.getSubTaskGroupType().name();
      subTaskData.errorString = taskInfo.getTaskDetails().get("errorString").asText();
      subTasks.add(subTaskData);
    }
    return subTasks;
  }

  private Map<UUID, List<CustomerTaskFormData>> fetchTasks(UUID customerUUID, UUID targetUUID) {
    Query<CustomerTask> customerTaskQuery = CustomerTask.find.query().where()
      .eq("customer_uuid", customerUUID)
      .orderBy("create_time desc");

    if (targetUUID != null) {
      customerTaskQuery.where().eq("target_uuid", targetUUID);
    }

    Set<CustomerTask> pendingTasks = customerTaskQuery.findSet();

    Map<UUID, List<CustomerTaskFormData>> taskListMap = new HashMap<>();

    for (CustomerTask task : pendingTasks) {
      try {
        CustomerTaskFormData taskData = new CustomerTaskFormData();

        JsonNode taskProgress = commissioner.getStatus(task.getTaskUUID());
        // If the task progress API returns error, we will log it and not add that task
        // to the task list for UI rendering.
        if (taskProgress.has("error")) {
          LOG.error("Error fetching Task Progress for " + task.getTaskUUID() +
            ", Error: " + taskProgress.get("error"));
        } else {
          taskData.percentComplete = taskProgress.get("percent").asInt();
          taskData.status = taskProgress.get("status").asText();
          taskData.id = task.getTaskUUID();
          taskData.title = task.getFriendlyDescription();
          taskData.createTime = task.getCreateTime();
          taskData.completionTime = task.getCompletionTime();
          taskData.target = task.getTarget().name();
          taskData.type = task.getType().getFriendlyName();
          taskData.targetUUID = task.getTargetUUID();

          List<CustomerTaskFormData> taskList = taskListMap.getOrDefault(task.getTargetUUID(),
            new ArrayList<>());
          taskList.add(taskData);
          taskListMap.put(task.getTargetUUID(), taskList);
        }
      } catch (RuntimeException e) {
        LOG.error("Error fetching Task Progress for " + task.getTaskUUID() +
          ", TaskInfo with that taskUUID not found");
      }
    }
    return taskListMap;
  }

  public Result list(UUID customerUUID) {
    Customer customer = Customer.get(customerUUID);

    if (customer == null) {
      ObjectNode responseJson = Json.newObject();
      responseJson.put("error", "Invalid Customer UUID: " + customerUUID);
      return badRequest(responseJson);
    }

    Map<UUID, List<CustomerTaskFormData>> taskList = fetchTasks(customerUUID, null);
    return ApiResponse.success(taskList);
  }

  public Result universeTasks(UUID customerUUID, UUID universeUUID) {
    Customer customer = Customer.get(customerUUID);
    if (customer == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Customer UUID: " + customerUUID);
    }
    Universe universe = Universe.getOrBadRequest(universeUUID);
    Map<UUID, List<CustomerTaskFormData>> taskList = fetchTasks(customerUUID,
      universe.universeUUID);
    return ApiResponse.success(taskList);
  }

  public Result status(UUID customerUUID, UUID taskUUID) {
    Customer customer = Customer.get(customerUUID);
    if (customer == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Customer UUID: " + customerUUID);
    }

    CustomerTask customerTask = CustomerTask.find.query().where()
      .eq("customer_uuid", customer.uuid)
      .eq("task_uuid", taskUUID)
      .findOne();

    if (customerTask == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Customer Task UUID: " + taskUUID);
    }

    try {
      ObjectNode responseJson = commissioner.getStatus(taskUUID);
      return ok(responseJson);
    } catch (RuntimeException e) {
      return ApiResponse.error(BAD_REQUEST, e.getMessage());
    }
  }

  public Result failedSubtasks(UUID customerUUID, UUID taskUUID) {
    Customer customer = Customer.get(customerUUID);
    if (customer == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Customer UUID: " + customerUUID);
    }

    CustomerTask customerTask = CustomerTask.find.query().where()
      .eq("customer_uuid", customer.uuid)
      .eq("task_uuid", taskUUID)
      .findOne();
    if (customerTask == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Customer Task UUID: " + taskUUID);
    }

    List<SubTaskFormData> failedSubTasks = fetchFailedSubTasks(taskUUID);
    ObjectNode responseJson = Json.newObject();
    responseJson.put("failedSubTasks", Json.toJson(failedSubTasks));
    return ok(responseJson);
  }

  public Result retryTask(UUID customerUUID, UUID taskUUID) {
    Customer customer = Customer.get(customerUUID);
    if (customer == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Customer UUID: " + customerUUID);
    }

    CustomerTask customerTask = CustomerTask.get(customer.uuid, taskUUID);

    if (customerTask == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Customer Task UUID: " + taskUUID);
    }

    TaskInfo taskInfo = TaskInfo.get(taskUUID);

    if (taskInfo == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Customer Task UUID: " + taskUUID);
    } else if (taskInfo.getTaskType() != TaskType.CreateUniverse) {
      String errMsg = String.format(
        "Invalid task type: %s. Only 'Create Universe' task retries are supported.",
        taskInfo.getTaskType().toString());
      return ApiResponse.error(BAD_REQUEST, errMsg);
    }

    JsonNode oldTaskParams = commissioner.getTaskDetails(taskUUID);
    UniverseDefinitionTaskParams params = Json.fromJson(
      oldTaskParams, UniverseDefinitionTaskParams.class);
    params.firstTry = false;
    Universe universe = Universe.getOrBadRequest(params.universeUUID);

    UUID newTaskUUID = commissioner.submit(taskInfo.getTaskType(), params);
    LOG.info("Submitted retry task to create universe for {}:{}, task uuid = {}.",
      universe.universeUUID, universe.name, newTaskUUID);

    // Add this task uuid to the user universe.
    CustomerTask.create(customer,
      universe.universeUUID,
      newTaskUUID,
      CustomerTask.TargetType.Universe,
      CustomerTask.TaskType.Create,
      universe.name);
    LOG.info("Saved task uuid " + newTaskUUID + " in customer tasks table for universe " +
      universe.universeUUID + ":" + universe.name);

    Audit.createAuditEntry(ctx(), request(), Json.toJson(params), newTaskUUID);
    return ApiResponse.success(new UniverseResp(universe, newTaskUUID));
  }
}
