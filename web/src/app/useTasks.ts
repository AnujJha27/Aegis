import { useEffect, useState } from "react";
import { api, type Task } from "./api";

export function useTasks() {
  const [tasks, setTasks] = useState<Task[]>([]);
  const [selectedTask, setSelectedTask] = useState<Task>();

  useEffect(() => {
    void api.tasks().then((loaded) => {
      setTasks(loaded);
      setSelectedTask(loaded[0]);
    });
  }, []);

  async function create(prompt: string) {
    const task = await api.createTask(prompt);
    setTasks((current) => [task, ...current]);
    setSelectedTask(task);
    return task;
  }

  return { tasks, selectedTask, setSelectedTask, create };
}
