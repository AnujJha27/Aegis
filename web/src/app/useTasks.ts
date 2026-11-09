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

  const update = useCallback((task: Task) => {
    setTasks((current) => current.map((item) => item.id === task.id ? { ...item, ...task } : item));
    setSelectedTask((current) => current?.id === task.id ? { ...current, ...task } : current);
  }, []);

  return { tasks, selectedTask, setSelectedTask, create, refresh, update };
}
