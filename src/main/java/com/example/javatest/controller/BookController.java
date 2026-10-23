package com.example.javatest.controller;

import com.example.javatest.entity.Book;
import com.example.javatest.service.BookService;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.web.bind.annotation.*;

import java.util.List;

@RestController
@RequestMapping("/api/books")
public class BookController {

    @Autowired
    private BookService bookService;

    @GetMapping
    public List<Book> findAll() {
        return bookService.findAll();
    }

    @GetMapping("/{id}")
    public Book findById(@PathVariable Long id) {
        return bookService.findById(id);
    }

    @PostMapping
    public String add(@RequestBody Book book) {
        bookService.add(book);
        return "新增成功";
    }

    @PutMapping
    public String update(@RequestBody Book book) {
        bookService.update(book);
        return "修改成功";
    }

    @DeleteMapping("/{id}")
    public String deleteById(@PathVariable Long id) {
        bookService.deleteById(id);
        return "删除成功";
    }
}
