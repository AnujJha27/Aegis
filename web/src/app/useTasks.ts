import { useCallback, useState } from "react";
import { api, type Task } from "./api";

export function useTasks() {
  const [tasks, setTasks] = useState<Task[]>([]);
  const [selectedTask, setSelectedTask] = useState<Task>();

  const refresh = useCallback(async () => {
    const loaded = await api.tasks();
    setTasks(loaded);
    setSelectedTask((current) => loaded.find((task) => task.id === current?.id) ?? loaded[0]);
  }, []);

  async function create(prompt: string) {
    const task = await api.createTask(prompt);
    setTasks((current) => [task, ...current]);
    setSelectedTask(task);
    return task;
  }

  return { tasks, selectedTask, setSelectedTask, create, refresh };
}
